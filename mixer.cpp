/*
 * Bermuda Syndrome engine rewrite
 * Copyright (C) 2007-2011 Gregory Montoir
 */

#if 0 // MIXER_SOFTWARE

#include "file.h"
#include "mixer.h"
#include "systemstub.h"
#ifdef BERMUDA_VORBIS
#include <vorbis/vorbisfile.h>
#endif

static const int _fracStepBits = 8;
static const int _sfxVolume = 256;
static const int _musicVolume = 192;

struct LockAudioStack {
	LockAudioStack(SystemStub *stub) : _stub(stub) {
		_stub->lockAudio();
	}
	~LockAudioStack() {
		_stub->unlockAudio();
	}
	SystemStub *_stub;
};

static void mixSample(int16_t &dst, int sample, int volume) {
	int pcm = dst + ((sample * volume) >> 8);
	if (pcm < -32768) {
		pcm = -32768;
	} else if (pcm > 32767) {
		pcm = 32767;
	}
	dst = (int16_t)pcm;
}

struct MixerChannel_Wav : MixerChannel {
	MixerChannel_Wav()
		: _buf(0), _bufSize(0), _bufReadOffset(0), _bufReadStep(0) {
	}

	virtual ~MixerChannel_Wav() {
		if (_buf) {
			free(_buf);
			_buf = 0;
		}
	}

	virtual bool load(File *f, int mixerSampleRate) {
		char buf[8];
		f->seek(8); // skip RIFF header
		f->read(buf, 8);
		if (memcmp(buf, "WAVEfmt ", 8) == 0) {
			f->readUint32LE(); // fmtLength
			int compression = f->readUint16LE();
			int channels = f->readUint16LE();
			int sampleRate = f->readUint32LE();
			f->readUint32LE(); // averageBytesPerSec
			f->readUint16LE(); // blockAlign
			_bitsPerSample = f->readUint16LE();
			if (compression != 1 ||
			    (channels != 1 && channels != 2) ||
			    (sampleRate != 11025 && sampleRate != 22050 && sampleRate != 44100) ||
			    (_bitsPerSample != 8 && _bitsPerSample != 16)) {
				warning("Unhandled wav/pcm format compression %d channels %d rate %d bits %d", compression, channels, sampleRate, _bitsPerSample);
				return false;
			}
			_stereo = (channels == 2);
			_bufReadStep = (sampleRate << _fracStepBits) / mixerSampleRate;
			f->read(buf, 4);
			if (memcmp(buf, "data", 4) == 0) {
				_bufSize = f->readUint32LE();
				_buf = (uint8_t *)malloc(_bufSize);
				if (_buf) {
					f->read(_buf, _bufSize);
					return true;
				}
			}
		}
		return false;
	}

	bool readSample(int16_t &sample) {
		switch (_bitsPerSample) {
		case 8:
			if ((_bufReadOffset >> _fracStepBits) >= _bufSize) { // end of buffer
				return false;
			}
			sample = (_buf[_bufReadOffset >> _fracStepBits] << 8) ^ 0x8000;
			break;
		case 16:
			if ((_bufReadOffset >> _fracStepBits) * 2 >= _bufSize) { // end of buffer
				return false;
			}
			sample = READ_LE_UINT16(&_buf[(_bufReadOffset >> _fracStepBits) * 2]);
			break;
		}
		_bufReadOffset += _bufReadStep;
		return true;
	}

	virtual int read(int16_t *dst, int samples) {
		for (int i = 0; i < samples; ++i) {
			int16_t sampleL = 0, sampleR;
			if (!readSample(sampleL)) {
				return i;
			}
			sampleR = sampleL;
			if (_stereo && !readSample(sampleR)) {
				return i;
			}
			mixSample(*dst++, sampleL, _sfxVolume);
			mixSample(*dst++, sampleR, _sfxVolume);
		}
		return samples;
	}

	uint8_t *_buf;
	int _bufSize;
	int _bufReadOffset;
	int _bufReadStep;
	int _bitsPerSample;
	bool _stereo;
};

#ifdef BERMUDA_VORBIS
static size_t file_vorbis_read_helper(void *ptr, size_t size, size_t nmemb, void *datasource) {
	if (size != 0 && nmemb != 0) {
		int n = ((File *)datasource)->read(ptr, size * nmemb);
		if (n > 0) {
			return n / size;
		}
	}
	return 0;
}

static int file_vorbis_seek_helper(void *datasource, ogg_int64_t offset, int whence) {
	((File *)datasource)->seek(offset, whence);
	return 0;
}

static int file_vorbis_close_helper(void *datasource) {
	((File *)datasource)->close();
	delete ((File *)datasource);
	return 0;
}

static long file_vorbis_tell_helper(void *datasource) {
	return ((File *)datasource)->tell();
}

struct MixerChannel_Vorbis : MixerChannel {
	MixerChannel_Vorbis()
		: _loop(true), _open(false), _readBuf(0), _readBufSize(0) {
	}

	virtual ~MixerChannel_Vorbis() {
		if (_open) {
			ov_clear(&_ovf);
		}
		free(_readBuf);
	}

	virtual bool load(File *f, int mixerSampleRate) {
		ov_callbacks ovcb;
		ovcb.read_func  = file_vorbis_read_helper;
		ovcb.seek_func  = file_vorbis_seek_helper;
		ovcb.close_func = file_vorbis_close_helper;
		ovcb.tell_func  = file_vorbis_tell_helper;
		if (ov_open_callbacks(f, &_ovf, 0, 0, ovcb) < 0) {
			warning("Invalid .ogg file");
			return false;
		}
		_open = true;
		vorbis_info *vi = ov_info(&_ovf, -1);
		if (vi->channels != 2 || vi->rate != mixerSampleRate) {
			warning("Unhandled ogg/pcm format ch %d rate %d", vi->channels, vi->rate);
			return false;
		}
		return true;
	}

	virtual int read(int16_t *dst, int samples) {
		int dstSize = samples * sizeof(int16_t) * 2;
		if (dstSize > _readBufSize) {
			_readBufSize = dstSize;
			free(_readBuf);
			_readBuf = (char *)malloc(_readBufSize);
			if (!_readBuf) {
				return 0;
			}
		}
		int readSize = 0;
		while (dstSize > 0) {
			int len = ov_read(&_ovf, _readBuf, dstSize, 0, 2, 1, 0);
			if (len < 0) {
				// error in decoder
				return 0;
			}
			if (len == 0) {
				if (_loop) {
					ov_raw_seek(&_ovf, 0);
					continue;
				}
				break;
			}
			// mix pcm data
			for (unsigned int i = 0; i < len / sizeof(int16_t); ++i) {
				const int16_t sample = (int16_t)READ_LE_UINT16(&_readBuf[i * 2]);
				mixSample(dst[readSize + i], sample, _musicVolume);
			}
			readSize += len / sizeof(int16_t);
			dstSize -= len;
		}
		return readSize;
	}

	OggVorbis_File _ovf;
	bool _loop;
	bool _open;
	char *_readBuf;
	int _readBufSize;
};
#endif

Mixer::Mixer(SystemStub *stub)
	: _stub(stub), _channelIdSeed(0), _open(false) {
	memset(_channels, 0, sizeof(_channels));
}

Mixer::~Mixer() {
	close();
	for (int i = 0; i < kMaxChannels; ++i) {
		if (_channels[i]) {
			delete _channels[i];
		}
	}
}

void Mixer::open() {
	if (!_open) {
		_stub->startAudio(Mixer::mixCallback, this);
		_open = true;
	}
}

void Mixer::close() {
	if (_open) {
		_stub->stopAudio();
		_open = false;
	}
}

void Mixer::startSound(File *f, int *id, MixerChannel *mc) {
	if (mc->load(f, _stub->getOutputSampleRate()) && bindChannel(mc, id)) {
		return;
	}
	*id = kDefaultSoundId;
	delete mc;
}

void Mixer::playSound(File *f, int *id) {
	debug(DBG_MIXER, "Mixer::playSound()");
	LockAudioStack las(_stub);
	startSound(f, id, new MixerChannel_Wav);
}

void Mixer::playMusic(File *f, int *id) {
	debug(DBG_MIXER, "Mixer::playMusic()");
#ifdef BERMUDA_VORBIS
	LockAudioStack las(_stub);
	startSound(f, id, new MixerChannel_Vorbis);
#endif
}

bool Mixer::isSoundPlaying(int id) {
	debug(DBG_MIXER, "Mixer::isSoundPlaying() 0x%X", id);
	if (id == kDefaultSoundId) {
		return false;
	}
	LockAudioStack las(_stub);
	int channel = getChannelFromSoundId(id);
	assert(channel >= 0 && channel < kMaxChannels);
	MixerChannel *mc = _channels[channel];
	return (mc && mc->id == id);
}

void Mixer::stopSound(int id) {
	debug(DBG_MIXER, "Mixer::stopSound() 0x%X", id);
	if (id == kDefaultSoundId) {
		return;
	}
	LockAudioStack las(_stub);
	int channel = getChannelFromSoundId(id);
	assert(channel >= 0 && channel < kMaxChannels);
	MixerChannel *mc = _channels[channel];
	if (mc && mc->id == id) {
		delete mc;
		_channels[channel] = 0;
	}
}

void Mixer::stopAll() {
	debug(DBG_MIXER, "Mixer::stopAll()");
	LockAudioStack las(_stub);
	for (int i = 0; i < kMaxChannels; ++i) {
		if (_channels[i]) {
			delete _channels[i];
			_channels[i] = 0;
		}
	}
}

void Mixer::setMusicMix(void *param, void (*mix)(void *, uint8_t *, int)) {
	if (mix) {
		_stub->startAudio(mix, param);
	} else {
		_stub->startAudio(mixCallback, this);
	}
}

void Mixer::mix(int16_t *buf, int len) {
	assert((len & 1) == 0);
	memset(buf, 0, len * sizeof(int16_t));
	for (int i = 0; i < kMaxChannels; ++i) {
		MixerChannel *mc = _channels[i];
		if (mc) {
			if (mc->read(buf, len / 2) <= 0) {
				delete mc;
				_channels[i] = 0;
			}
		}
	}
}

void Mixer::mixCallback(void *param, uint8_t *buf, int len) {
	assert((len & 1) == 0);
	((Mixer *)param)->mix((int16_t *)buf, len / 2);
}

int Mixer::generateSoundId(int channel) {
	++_channelIdSeed;
	_channelIdSeed &= 0xFFFF;
	assert(channel >= 0 && channel < 16);
	return (_channelIdSeed << 4) | channel;
}

int Mixer::getChannelFromSoundId(int id) {
	return id & 15;
}

bool Mixer::bindChannel(MixerChannel *mc, int *id) {
	for (int i = 0; i < kMaxChannels; ++i) {
		if (!_channels[i]) {
			_channels[i] = mc;
			*id = mc->id = generateSoundId(i);
			return true;
		}
	}
	return false;
}

void Mixer::unbindChannel(int channel) {
	assert(channel >= 0 && channel < kMaxChannels);
	if (_channels[channel]) {
		delete _channels[channel];
		_channels[channel] = 0;
	}
}
#else

#include <SDL.h>
#include <SDL_mixer.h>
#include "file.h"
#include "mixer.h"
#include "util.h"

struct MixerImpl {

	static const int kMixFreq = 22050;
	static const int kMixBufSize = 4096;
	static const int kChannels = 4;

	bool _isOpen;
	Mix_Music *_music;

	MixerImpl()
		: _isOpen(false) {
		_music = 0;
	}

	virtual ~MixerImpl() {
	}

	virtual void open() {
		if (_isOpen) {
			return;
		}
		Mix_Init(MIX_INIT_OGG | MIX_INIT_FLUIDSYNTH);
		if (Mix_OpenAudio(kMixFreq, AUDIO_S16SYS, 2, kMixBufSize) < 0) {
			warning("Mix_OpenAudio failed: %s", Mix_GetError());
		}
		Mix_AllocateChannels(kChannels);
		_isOpen = true;
	}
	virtual void close() {
		if (!_isOpen) {
			return;
		}
		stopAll();
		Mix_CloseAudio();
		Mix_Quit();
		_isOpen = false;
	}

	virtual void playSound(const char *path, int *id) {
		debug(DBG_MIXER, "MixerImpl::playSound() path '%s'", path);
		Mix_Chunk *chunk = Mix_LoadWAV(path);
		if (chunk) {
			*id = Mix_PlayChannel(-1, chunk, 0);
		} else {
			*id = -1;
		}
	}
	virtual void playSoundMusic(const char *path, int *id) {
		debug(DBG_MIXER, "MixerImpl::playSoundMusic() path '%s'", path);
		playMusic(path);
		*id = -1;
	}
	virtual bool isPlaying(int id) {
		return Mix_Playing(id) != 0;
	}
	virtual void stopSound(int id) {
		debug(DBG_MIXER, "MixerImpl::stopSound()");
		Mix_HaltChannel(id);
		// Mix_FreeChunk
	}

	void playMusic(const char *path) {
		stopMusic();
		_music = Mix_LoadMUS(path);
		if (_music) {
			Mix_VolumeMusic(MIX_MAX_VOLUME / 2);
			Mix_PlayMusic(_music, 0);
		} else {
			warning("Failed to load music '%s', %s", path, Mix_GetError());
		}
	}
	void stopMusic() {
		Mix_HaltMusic();
		Mix_FreeMusic(_music);
		_music = 0;
	}
	void setMusicMix(void *param, void (*mix)(void *, uint8_t *, int)) {
		Mix_HookMusic(mix, param);
	}

	virtual void stopAll() {
		debug(DBG_MIXER, "MixerImpl::stopAll()");
		Mix_HaltChannel(-1);
		stopMusic();
	}
};

Mixer::Mixer(SystemStub *stub)
	: _stub(stub) {
	_impl = new MixerImpl;
}

Mixer::~Mixer() {
	delete _impl;
}

void Mixer::open() {
	_impl->open();
}

void Mixer::close() {
	_impl->close();
}

void Mixer::playSound(File *f, int *id) {
	_impl->playSound(f->_path, id);
}

void Mixer::playMusic(File *f, int *id) {
	_impl->playSoundMusic(f->_path, id);
}

bool Mixer::isSoundPlaying(int id) {
	return _impl->isPlaying(id);
}

void Mixer::stopSound(int id) {
	_impl->stopSound(id);
}

void Mixer::stopAll() {
	_impl->stopAll();
}

void Mixer::setMusicMix(void *param, void (*mix)(void *, uint8_t *, int)) {
	if (mix) {
		_impl->open();
		_impl->setMusicMix(param, mix);
	} else {
		_impl->setMusicMix(0, 0);
	}
}

#endif
