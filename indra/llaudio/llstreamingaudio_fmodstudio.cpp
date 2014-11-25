/** 
 * @file streamingaudio_fmodstudio.cpp
 * @brief LLStreamingAudio_FMODSTUDIO implementation
 *
 * $LicenseInfo:firstyear=2009&license=viewergpl$
 * 
 * Copyright (c) 2009, Linden Research, Inc.
 * 
 * Second Life Viewer Source Code
 * The source code in this file ("Source Code") is provided by Linden Lab
 * to you under the terms of the GNU General Public License, version 2.0
 * ("GPL"), unless you have obtained a separate licensing agreement
 * ("Other License"), formally executed by you and Linden Lab.  Terms of
 * the GPL can be found in doc/GPL-license.txt in this distribution, or
 * online at http://secondlifegrid.net/programs/open_source/licensing/gplv2
 * 
 * There are special exceptions to the terms and conditions of the GPL as
 * it is applied to this Source Code. View the full text of the exception
 * in the file doc/FLOSS-exception.txt in this software distribution, or
 * online at
 * http://secondlifegrid.net/programs/open_source/licensing/flossexception
 * 
 * By copying, modifying or distributing this software, you acknowledge
 * that you have read and understood your obligations described above,
 * and agree to abide by those obligations.
 * 
 * ALL LINDEN LAB SOURCE CODE IS PROVIDED "AS IS." LINDEN LAB MAKES NO
 * WARRANTIES, EXPRESS, IMPLIED OR OTHERWISE, REGARDING ITS ACCURACY,
 * COMPLETENESS OR PERFORMANCE.
 * $/LicenseInfo$
 */

#include "linden_common.h"

#include "llmath.h"
#include "llthread.h"

#include "fmod.hpp"
#include "fmod_errors.h"

#include "llstreamingaudio_fmodstudio.h"

inline bool Check_FMOD_Error(FMOD_RESULT result, const char *string)
{
	if (result == FMOD_OK)
		return false;
	LL_WARNS("AudioImpl") << string << " Error: " << FMOD_ErrorString(result) << LL_ENDL;
	return true;
}

class LLAudioStreamManagerFMODSTUDIO
{
public:
	LLAudioStreamManagerFMODSTUDIO(FMOD::System *system, FMOD::ChannelGroup *group, const std::string& url);
	FMOD::Channel* startStream();
	bool stopStream(); // Returns true if the stream was successfully stopped.
	bool ready();

	const std::string& getURL() 	{ return mInternetStreamURL; }

	FMOD_OPENSTATE getOpenState(unsigned int* percentbuffered=NULL, bool* starving=NULL, bool* diskbusy=NULL);
protected:
	FMOD::System* mSystem;
	FMOD::Channel* mStreamChannel;
	FMOD::Sound* mInternetStream;
	FMOD::ChannelGroup* mChannelGroup;
	bool mReady;

	std::string mInternetStreamURL;
};

LLGlobalMutex gWaveDataMutex;	//Just to be extra strict.
const U32 WAVE_BUFFER_SIZE = 1024;
U32 gWaveBufferMinSize = 0;
F32 gWaveDataBuffer[WAVE_BUFFER_SIZE] = { 0.f };
U32 gWaveDataBufferSize = 0;

FMOD_RESULT F_CALLBACK waveDataCallback(FMOD_DSP_STATE *dsp_state, float *inbuffer, float *outbuffer, unsigned int length, int inchannels, int *outchannels)
{
	if (!length || !inchannels)
		return FMOD_OK;
	memcpy(outbuffer, inbuffer, length * inchannels * sizeof(float));

	static std::vector<F32> local_buf;
	if (local_buf.size() < length)
		local_buf.resize(length, 0.f);

	for (U32 i = 0; i < length; ++i)
	{
		F32 total = 0.f;
		for (S32 j = 0; j < inchannels; ++j)
		{
			total += inbuffer[i*inchannels + j];
		}
		local_buf[i] = total / inchannels;
	}

	{
		LLMutexLock lock(gWaveDataMutex);

		for (U32 i = length; i > 0; --i)
		{
			if (++gWaveDataBufferSize > WAVE_BUFFER_SIZE)
			{
				if (gWaveBufferMinSize)
					memcpy(gWaveDataBuffer + WAVE_BUFFER_SIZE - gWaveBufferMinSize, gWaveDataBuffer, gWaveBufferMinSize * sizeof(float));
				gWaveDataBufferSize = 1 + gWaveBufferMinSize;
			}
			gWaveDataBuffer[WAVE_BUFFER_SIZE - gWaveDataBufferSize] = local_buf[i - 1];
		}
	}
	
	return FMOD_OK;
}

//---------------------------------------------------------------------------
// Internet Streaming
//---------------------------------------------------------------------------
LLStreamingAudio_FMODSTUDIO::LLStreamingAudio_FMODSTUDIO(FMOD::System *system) :
	mSystem(system),
	mCurrentInternetStreamp(NULL),
	mFMODInternetStreamChannelp(NULL),
	mGain(1.0f),
	mMetaData(NULL)
{
	FMOD_RESULT result;

	// Number of milliseconds of audio to buffer for the audio card.
	// Must be larger than the usual Second Life frame stutter time.
	const U32 buffer_seconds = 10;		//sec
	const U32 estimated_bitrate = 128;	//kbit/sec
	result = mSystem->setStreamBufferSize(estimated_bitrate * buffer_seconds * 128/*bytes/kbit*/, FMOD_TIMEUNIT_RAWBYTES);
	Check_FMOD_Error(result, "FMOD::System::setStreamBufferSize");

	// Here's where we set the size of the network buffer and some buffering 
	// parameters.  In this case we want a network buffer of 16k, we want it 
	// to prebuffer 40% of that when we first connect, and we want it 
	// to rebuffer 80% of that whenever we encounter a buffer underrun.

	// Leave the net buffer properties at the default.
	//FSOUND_Stream_Net_SetBufferProperties(20000, 40, 80);

	result = system->createChannelGroup("stream", &mStreamGroup);
	Check_FMOD_Error(result,"FMOD::System::createChannelGroup");

	FMOD_DSP_DESCRIPTION dspdesc;
	memset(&dspdesc, 0, sizeof(FMOD_DSP_DESCRIPTION));	//Zero out everything
	dspdesc.pluginsdkversion = FMOD_PLUGIN_SDK_VERSION;
	strncpy(dspdesc.name, "Waveform", sizeof(dspdesc.name));
	dspdesc.numoutputbuffers = 1;
	dspdesc.read = &waveDataCallback; //Assign callback.

	result = system->createDSP(&dspdesc, &mStreamDSP);
	Check_FMOD_Error(result, "FMOD::System::createDSPByType");
	result = mStreamGroup->addDSP(FMOD_CHANNELCONTROL_DSP_TAIL, mStreamDSP);
	Check_FMOD_Error(result, "FMOD::ChannelGroup::addDSP");
	mStreamDSP->setActive(false);
}


LLStreamingAudio_FMODSTUDIO::~LLStreamingAudio_FMODSTUDIO()
{
	stop();
	for (U32 i = 0; i < 100; ++i)
	{
		if (releaseDeadStreams())
			break;
		ms_sleep(10);
	}

	if (mStreamGroup)
	{
		if (mStreamDSP)
			mStreamGroup->removeDSP(mStreamDSP);
		mStreamGroup->release();
	}
	if (mStreamDSP)
		mStreamDSP->release();
}


void LLStreamingAudio_FMODSTUDIO::start(const std::string& url)
{
	//if (!mInited)
	//{
	//	llwarns << "startInternetStream before audio initialized" << llendl;
	//	return;
	//}

	// "stop" stream but don't clear url, etc. in case url == mInternetStreamURL
	stop();

	if (!url.empty())
	{
		if(mDeadStreams.empty())
		{
			llinfos << "Starting internet stream: " << url << llendl;
			mCurrentInternetStreamp = new LLAudioStreamManagerFMODSTUDIO(mSystem, mStreamGroup, url);
			mURL = url;
			mMetaData = new LLSD;
		}
		else
		{
			llinfos << "Deferring stream load until buffer release: " << url << llendl;
			mPendingURL = url;
		}
	}
	else
	{
		llinfos << "Set internet stream to null" << llendl;
		mURL.clear();
	}
}

enum utf_endian_type_t
{
	UTF16LE,
	UTF16BE,
	UTF16
};

std::string utf16input_to_utf8(char* input, U32 len, utf_endian_type_t type)
{
	if (type == UTF16)
	{
		type = UTF16BE;	//Default
		if (len > 2)
		{
			//Parse and strip BOM.
			if ((input[0] == 0xFE && input[1] == 0xFF) || 
				(input[0] == 0xFF && input[1] == 0xFE))
			{
				input += 2;
				len -= 2;
				type = input[0] == 0xFE ? UTF16BE : UTF16LE;
			}
		}
	}
	llutf16string out_16((U16*)input, len / 2);
	if (len % 2)
	{
		out_16.push_back((input)[len - 1] << 8);
	}
	if (type == UTF16BE)
	{
		for (llutf16string::iterator i = out_16.begin(); i < out_16.end(); ++i)
		{
			llutf16string::value_type v = *i;
			*i = ((v & 0x00FF) << 8) | ((v & 0xFF00) >> 8);
		}
	}
	return utf16str_to_utf8str(out_16);
}

void LLStreamingAudio_FMODSTUDIO::update()
{
	if (!releaseDeadStreams())
	{
		llassert_always(mCurrentInternetStreamp == NULL);
		return;
	}

	if(!mPendingURL.empty())
	{
		llassert_always(mCurrentInternetStreamp == NULL);
		llinfos << "Starting internet stream: " << mPendingURL << llendl;
		mCurrentInternetStreamp = new LLAudioStreamManagerFMODSTUDIO(mSystem,mStreamGroup, mPendingURL);
		mURL = mPendingURL;
		mMetaData = new LLSD;
		mPendingURL.clear();
	}

	// Don't do anything if there are no streams playing
	if (!mCurrentInternetStreamp)
	{
		return;
	}

	unsigned int progress;
	bool starving;
	bool diskbusy;
	FMOD_OPENSTATE open_state = mCurrentInternetStreamp->getOpenState(&progress, &starving, &diskbusy);

	if (open_state == FMOD_OPENSTATE_READY)
	{
		// Stream is live

		// start the stream if it's ready
		if (!mFMODInternetStreamChannelp &&
			(mFMODInternetStreamChannelp = mCurrentInternetStreamp->startStream()))
		{
			// Reset volume to previously set volume
			setGain(getGain());
			if (mStreamDSP)
				mStreamDSP->setActive(true);
			mFMODInternetStreamChannelp->setPaused(false);
		}
	}
	else if(open_state == FMOD_OPENSTATE_ERROR)
	{
		stop();
		return;
	}

	if(mFMODInternetStreamChannelp)
	{
		if(!mMetaData)
			mMetaData = new LLSD;

		FMOD::Sound *sound = NULL;
		
		if(mFMODInternetStreamChannelp->getCurrentSound(&sound) == FMOD_OK && sound)
		{
			FMOD_TAG tag;
			S32 tagcount, dirtytagcount;
			if(sound->getNumTags(&tagcount, &dirtytagcount) == FMOD_OK && dirtytagcount)
			{
				mMetaData->clear();

				for(S32 i = 0; i < tagcount; ++i)
				{
					if(sound->getTag(NULL, i, &tag)!=FMOD_OK)
						continue;
					std::string name = tag.name;
					switch(tag.type)	//Crappy tag translate table.
					{
					case(FMOD_TAGTYPE_ID3V2):
						if (!LLStringUtil::compareInsensitive(name, "TIT2")) name = "TITLE";
						else if(name == "TPE1") name = "ARTIST";
						break;
					case(FMOD_TAGTYPE_ASF):
						if (!LLStringUtil::compareInsensitive(name, "Title")) name = "TITLE";
						else if (!LLStringUtil::compareInsensitive(name, "WM/AlbumArtist")) name = "ARTIST";
						break;
					case(FMOD_TAGTYPE_FMOD):
						if (!LLStringUtil::compareInsensitive(name, "Sample Rate Change"))
						{
							llinfos << "Stream forced changing sample rate to " << *((float *)tag.data) << llendl;
							mFMODInternetStreamChannelp->setFrequency(*((float *)tag.data));
						}
						continue;
					default:
						if (!LLStringUtil::compareInsensitive(name, "TITLE") ||
							!LLStringUtil::compareInsensitive(name, "ARTIST"))
							LLStringUtil::toUpper(name);
						break;
					}

					switch(tag.datatype)
					{
						case(FMOD_TAGDATATYPE_INT):
							(*mMetaData)[name]=*(LLSD::Integer*)(tag.data);
							llinfos << tag.name << ": " << *(int*)(tag.data) << llendl;
							break;
						case(FMOD_TAGDATATYPE_FLOAT):
							(*mMetaData)[name]=*(LLSD::Float*)(tag.data);
							llinfos << tag.name << ": " << *(float*)(tag.data) << llendl;
							break;
						case(FMOD_TAGDATATYPE_STRING):
						{
							std::string out = rawstr_to_utf8(std::string((char*)tag.data,tag.datalen));
							if (out.length() && out.back() == 0)
								out.pop_back();
							(*mMetaData)[name]=out;
							llinfos << tag.name << "(RAW): " << out << llendl;
						}
							break;
						case(FMOD_TAGDATATYPE_STRING_UTF8) :
						{
							U8 offs = 0;
							if (tag.datalen > 3 && ((char*)tag.data)[0] == 0xEF && ((char*)tag.data)[1] == 0xBB && ((char*)tag.data)[2] == 0xBF)
								offs = 3;
							std::string out((char*)tag.data + offs, tag.datalen - offs);
							if (out.length() && out.back() == 0)
								out.pop_back();
							(*mMetaData)[name] = out;
							llinfos << tag.name << "(UTF8): " << out << llendl;
						}
							break;
						case(FMOD_TAGDATATYPE_STRING_UTF16):
						{
							std::string out = utf16input_to_utf8((char*)tag.data, tag.datalen, UTF16);
							if (out.length() && out.back() == 0)
								out.pop_back();
							(*mMetaData)[name] = out;
							llinfos << tag.name << "(UTF16): " << out << llendl;
						}
							break;
						case(FMOD_TAGDATATYPE_STRING_UTF16BE):
						{
							std::string out = utf16input_to_utf8((char*)tag.data, tag.datalen, UTF16BE);
							if (out.length() && out.back() == 0)
								out.pop_back();
							(*mMetaData)[name] = out;
							llinfos << tag.name << "(UTF16BE): " << out << llendl;
						}
						default:
							break;
					}
				}
			}
			if(starving)
			{
				bool paused = false;
				mFMODInternetStreamChannelp->getPaused(&paused);
				if(!paused)
				{
					llinfos << "Stream starvation detected! Pausing stream until buffer nearly full." << llendl;
					llinfos << "  (diskbusy="<<diskbusy<<")" << llendl;
					llinfos << "  (progress="<<progress<<")" << llendl;
					mFMODInternetStreamChannelp->setPaused(true);
				}
			}
			else if(progress > 80)
			{
				mFMODInternetStreamChannelp->setPaused(false);
			}
		}
	}
}

void LLStreamingAudio_FMODSTUDIO::stop()
{
	mPendingURL.clear();

	if(mMetaData)
	{
		delete mMetaData;
		mMetaData = NULL;
	}
	
	if (mStreamDSP)
	{
		mSystem->lockDSP();
		mStreamDSP->setActive(false);
		gWaveDataBufferSize = 0;
		mSystem->unlockDSP();
	}

	if (mFMODInternetStreamChannelp)
	{
		mFMODInternetStreamChannelp->setPaused(true);
		mFMODInternetStreamChannelp->setPriority(0);
		mFMODInternetStreamChannelp = NULL;
	}

	if (mCurrentInternetStreamp)
	{
		llinfos << "Stopping internet stream: " << mCurrentInternetStreamp->getURL() << llendl;
		if (mCurrentInternetStreamp->stopStream())
		{
			delete mCurrentInternetStreamp;
		}
		else
		{
			llwarns << "Pushing stream to dead list: " << mCurrentInternetStreamp->getURL() << llendl;
			mDeadStreams.push_back(mCurrentInternetStreamp);
		}
		mCurrentInternetStreamp = NULL;
		//mURL.clear();
	}
}

void LLStreamingAudio_FMODSTUDIO::pause(int pauseopt)
{
	if (pauseopt < 0)
	{
		pauseopt = mCurrentInternetStreamp ? 1 : 0;
	}

	if (pauseopt)
	{
		if (mCurrentInternetStreamp)
		{
			stop();
		}
	}
	else
	{
		start(getURL());
	}
}


// A stream is "playing" if it has been requested to start.  That
// doesn't necessarily mean audio is coming out of the speakers.
int LLStreamingAudio_FMODSTUDIO::isPlaying()
{
	if (mCurrentInternetStreamp)
	{
		return 1; // Active and playing
	}
	else if (!mURL.empty() || !mPendingURL.empty())
	{
		return 2; // "Paused"
	}
	else
	{
		return 0;
	}
}


F32 LLStreamingAudio_FMODSTUDIO::getGain()
{
	return mGain;
}


std::string LLStreamingAudio_FMODSTUDIO::getURL()
{
	return mURL;
}


void LLStreamingAudio_FMODSTUDIO::setGain(F32 vol)
{
	mGain = vol;

	if (mFMODInternetStreamChannelp)
	{
		vol = llclamp(vol * vol, 0.f, 1.f);	//should vol be squared here?

		mFMODInternetStreamChannelp->setVolume(vol);
	}
}

/*virtual*/ bool LLStreamingAudio_FMODSTUDIO::getWaveData(float* arr, S32 count, S32 stride/*=1*/)
{
	if (count > (WAVE_BUFFER_SIZE / 2))
		LL_ERRS("AudioImpl") << "Count=" << count << " exceeds WAVE_BUFFER_SIZE/2=" << WAVE_BUFFER_SIZE << LL_ENDL;

	if(!mFMODInternetStreamChannelp || !mCurrentInternetStreamp)
		return false;

	bool muted = false;
	mFMODInternetStreamChannelp->getMute(&muted);
	if(muted)
		return false;
	{
		U32 buff_size;
		{
			LLMutexLock lock(gWaveDataMutex);
			gWaveBufferMinSize = count;
			buff_size = gWaveDataBufferSize;
			if (!buff_size)
				return false;
			memcpy(arr, gWaveDataBuffer + WAVE_BUFFER_SIZE - buff_size, llmin(U32(count), buff_size) * sizeof(float));
		}
		if (buff_size < U32(count))
			memset(arr + buff_size, 0, (count - buff_size) * sizeof(float));
	}
	return true;
}

///////////////////////////////////////////////////////
// manager of possibly-multiple internet audio streams

LLAudioStreamManagerFMODSTUDIO::LLAudioStreamManagerFMODSTUDIO(FMOD::System *system, FMOD::ChannelGroup *group, const std::string& url) :
	mSystem(system),
	mStreamChannel(NULL),
	mInternetStream(NULL),
	mChannelGroup(group),
	mReady(false)
{
	mInternetStreamURL = url;

	FMOD_RESULT result = mSystem->createStream(url.c_str(), FMOD_2D | FMOD_NONBLOCKING | FMOD_IGNORETAGS, 0, &mInternetStream);

	if (result!= FMOD_OK)
	{
		llwarns << "Couldn't open fmod stream, error "
			<< FMOD_ErrorString(result)
			<< llendl;
		mReady = false;
		return;
	}

	mReady = true;
}

FMOD::Channel *LLAudioStreamManagerFMODSTUDIO::startStream()
{
	// We need a live and opened stream before we try and play it.
	if (!mInternetStream || getOpenState() != FMOD_OPENSTATE_READY)
	{
		llwarns << "No internet stream to start playing!" << llendl;
		return NULL;
	}

	if(mStreamChannel)
		return mStreamChannel;	//Already have a channel for this stream.

	mSystem->playSound(mInternetStream, mChannelGroup, true, &mStreamChannel);
	return mStreamChannel;
}

bool LLAudioStreamManagerFMODSTUDIO::stopStream()
{
	if (mInternetStream)
	{
		bool close = true;
		switch (getOpenState())
		{
		case FMOD_OPENSTATE_CONNECTING:
			close = false;
			break;
		default:
			close = true;
		}

		if (close && mInternetStream->release() == FMOD_OK)
		{
			mStreamChannel = NULL;
			mInternetStream = NULL;
			return true;
		}
		else
		{
			return false;
		}
	}
	else
	{
		return true;
	}
}

FMOD_OPENSTATE LLAudioStreamManagerFMODSTUDIO::getOpenState(unsigned int* percentbuffered, bool* starving, bool* diskbusy)
{
	FMOD_OPENSTATE state;
	mInternetStream->getOpenState(&state, percentbuffered, starving, diskbusy);
	return state;
}

void LLStreamingAudio_FMODSTUDIO::setBufferSizes(U32 streambuffertime, U32 decodebuffertime)
{
	mSystem->setStreamBufferSize(streambuffertime/1000*128*128, FMOD_TIMEUNIT_RAWBYTES);
	FMOD_ADVANCEDSETTINGS settings;
	memset(&settings,0,sizeof(settings));
	settings.cbSize=sizeof(settings);
	settings.defaultDecodeBufferSize = decodebuffertime;//ms
	mSystem->setAdvancedSettings(&settings);
}

bool LLStreamingAudio_FMODSTUDIO::releaseDeadStreams()
{
	// Kill dead internet streams, if possible
	std::list<LLAudioStreamManagerFMODSTUDIO *>::iterator iter;
	for (iter = mDeadStreams.begin(); iter != mDeadStreams.end();)
	{
		LLAudioStreamManagerFMODSTUDIO *streamp = *iter;
		if (streamp->stopStream())
		{
			llinfos << "Closed dead stream" << llendl;
			delete streamp;
			mDeadStreams.erase(iter++);
		}
		else
		{
			iter++;
		}
	}

	return mDeadStreams.empty();
}