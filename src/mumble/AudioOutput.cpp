// Copyright 2007-2023 The Mumble Developers. All rights reserved.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

#include "AudioOutput.h"

#include "AudioInput.h"
#include "AudioOutputSample.h"
#include "AudioOutputSpeech.h"
#include "Channel.h"
#include "ChannelListenerManager.h"
#include "Log.h"
#include "PluginManager.h"
#include "ServerHandler.h"
#include "Timer.h"
#include "User.h"
#include "Utils.h"
#include "VoiceRecorder.h"
#include "Global.h"

#include <cassert>
#include <cmath>
#include <cstdio>

// Remember that we cannot use static member classes that are not pointers, as the constructor
// for AudioOutputRegistrar() might be called before they are initialized, as the constructor
// is called from global initialization.
// Hence, we allocate upon first call.

QMap< QString, AudioOutputRegistrar * > *AudioOutputRegistrar::qmNew;
QString AudioOutputRegistrar::current = QString();

AudioOutputRegistrar::AudioOutputRegistrar(const QString &n, int p) : name(n), priority(p) {
	if (!qmNew)
		qmNew = new QMap< QString, AudioOutputRegistrar * >();
	qmNew->insert(name, this);
}

AudioOutputRegistrar::~AudioOutputRegistrar() {
	qmNew->remove(name);
}

AudioOutputPtr AudioOutputRegistrar::newFromChoice(QString choice) {
	if (!qmNew)
		return AudioOutputPtr();

	if (!choice.isEmpty() && qmNew->contains(choice)) {
		Global::get().s.qsAudioOutput = choice;
		current                       = choice;
		return AudioOutputPtr(qmNew->value(choice)->create());
	}
	choice = Global::get().s.qsAudioOutput;
	if (qmNew->contains(choice)) {
		current = choice;
		return AudioOutputPtr(qmNew->value(choice)->create());
	}

	AudioOutputRegistrar *r = nullptr;
	foreach (AudioOutputRegistrar *aor, *qmNew)
		if (!r || (aor->priority > r->priority))
			r = aor;
	if (r) {
		current = r->name;
		return AudioOutputPtr(r->create());
	}
	return AudioOutputPtr();
}

bool AudioOutputRegistrar::canMuteOthers() const {
	return false;
}

bool AudioOutputRegistrar::usesOutputDelay() const {
	return true;
}

bool AudioOutputRegistrar::canExclusive() const {
	return false;
}

AudioOutput::AudioOutput() /* : envSources(5), envSourceBuffers(5) */ {
	QObject::connect(this, &AudioOutput::bufferInvalidated, this, &AudioOutput::handleInvalidatedBuffer);
	QObject::connect(this, &AudioOutput::bufferPositionChanged, this, &AudioOutput::handlePositionedBuffer);


	envManager.BeginSetup();
	//freeEnv     = envManager.CreateEnvironment< BRTEnvironmentModel::CFreeFieldEnvironmentModel >("env");
	envListener = envManager.CreateListenerModel< BRTListenerModel::CListenerHRTFModel> ("listenerModel");
	listener    = envManager.CreateListener< BRTBase::CListener >("listener");
	//envListener->ConnectEnvironmentModel("env");
	listener->ConnectListenerModel("listenerModel");
	envManager.EndSetup();
	newInstance = true;

	//logFile = std::ofstream("speakerLog.txt");

	//hrtf_loaded = std::make_shared< BRTServices::CHRTF >();
}

AudioOutput::~AudioOutput() {
	//logFile.close();
	bRunning = false;
	wait();
	wipe();

	envListener->RemoveHRTF();
	hrtf_loaded.reset();
	envManager.RemoveListener("listener");
	globalParameters.SetSampleRate(DEFAULT_SAMPLE_RATE);
	initialized = false;

	delete[] fSpeakers;
	delete[] fSpeakerVolume;
	delete[] bSpeakerPositional;
	delete[] fStereoPanningFactor;
}

// Here's the theory.
// We support sound "bloom"ing. That is, if sound comes directly from the left, if it is sufficiently
// close, we'll hear it full intensity from the left side, and "bloom" intensity from the right side.

float AudioOutput::calcGain(float dotproduct, float distance) {
	// dotproduct is in the range [-1, 1], thus we renormalize it to the range [0, 1]
	float dotfactor = (dotproduct + 1.0f) / 2.0f;

	// Volume on the ear opposite to the sound should never reach 0 in the real world.
	// Therefore, we define the minimum volume as 1/4th of the theoretical maximum (ignoring
	// the sound direction but taking distance into account) for _any_ ear.
	const float offset = (1.0f - dotfactor) * 0.25f;
	dotfactor += offset;

	float att;

	if (distance < 0.01f) {
		// Listener is "inside" source -> no attenuation
		// Without this extra check, we would have a dotfactor of 0.5
		// despite being numerically inside the source leading to a loss
		// of volume.
		att = 1.0f;
	} else if (Global::get().s.fAudioMaxDistVolume > 0.99f) {
		// User selected no distance attenuation
		att = std::min(1.0f, dotfactor + Global::get().s.fAudioBloom);
	} else if (distance < Global::get().s.fAudioMinDistance) {
		// Fade in blooming as soon as the sound source enters fAudioMinDistance and increase it to its full
		// capability when the audio source is at the same position as the local player
		float bloomfac = Global::get().s.fAudioBloom * (1.0f - distance / Global::get().s.fAudioMinDistance);

		att = std::min(1.0f, bloomfac + dotfactor);
	} else {
		float datt;

		if (distance >= Global::get().s.fAudioMaxDistance) {
			datt = Global::get().s.fAudioMaxDistVolume;
		} else {
			float mvol = Global::get().s.fAudioMaxDistVolume;
			if (mvol < 0.005f) {
				mvol = 0.005f;
			}

			float drel = (distance - Global::get().s.fAudioMinDistance)
						 / (Global::get().s.fAudioMaxDistance - Global::get().s.fAudioMinDistance);
			datt = powf(10.0f, log10f(mvol) * drel);
		}

		att = datt * dotfactor;
	}
	return att;
}

void AudioOutput::wipe() {
	// We need to remove all buffers from the qmOutputs map.
	// However, the removeBuffer calls a signal-slot mechanism
	// asynchronously. Doing that while iterating over the map
	// will cause a concurrent modification

	QList< AudioOutputBuffer * > list;

	{
		QReadLocker locker(&qrwlOutputs);
		for (AudioOutputBuffer *buffer : qmOutputs) {
			list.append(buffer);
		}
	}

	for (AudioOutputBuffer *buffer : list) {
		removeBuffer(buffer);
	}
}

const float *AudioOutput::getSpeakerPos(unsigned int &speakers) {
	if ((iChannels > 0) && fSpeakers) {
		speakers = iChannels;
		return fSpeakers;
	}
	return nullptr;
}

void AudioOutput::addFrameToBuffer(ClientUser *sender, const Mumble::Protocol::AudioData &audioData) {
	if (iChannels == 0) {
		return;
	}

	qrwlOutputs.lockForRead();
	// qmOutputs is a map of users and their AudioOutputSpeech objects, which will be created when audio from that user
	// is received. It also contains AudioOutputSample objects with various other non-speech sounds.
	// This map will be iterated in mix(). After the speech or sample audio is finished, the AudioOutputBuffer object
	// will be removed from this map and deleted.
	AudioOutputSpeech *speech = qobject_cast< AudioOutputSpeech * >(qmOutputs.value(sender));

	if (!speech || (speech->m_codec != audioData.usedCodec)) {
		qrwlOutputs.unlock();

		if (speech) {
			removeBuffer(static_cast< AudioOutputBuffer * >(speech));
		}

		while ((iMixerFreq == 0) && isAlive()) {
			QThread::yieldCurrentThread();
		}

		if (!iMixerFreq) {
			return;
		}

		qrwlOutputs.lockForWrite();

		speech = new AudioOutputSpeech(sender, iMixerFreq, audioData.usedCodec, iBufferSize);
		qmOutputs.replace(sender, speech);
	}

	speech->addFrameToBuffer(audioData);

	qrwlOutputs.unlock();
}

void AudioOutput::handleInvalidatedBuffer(AudioOutputBuffer *buffer) {
	QWriteLocker locker(&qrwlOutputs);
	for (auto iter = qmOutputs.begin(); iter != qmOutputs.end(); ++iter) {
		if (iter.value() == buffer) {
			qmOutputs.erase(iter);
			delete buffer;
			break;
		}
	}
}

void AudioOutput::handlePositionedBuffer(AudioOutputBuffer *buffer, float x, float y, float z) {
	QWriteLocker locker(&qrwlOutputs);
	for (auto iter = qmOutputs.begin(); iter != qmOutputs.end(); ++iter) {
		if (iter.value() == buffer) {
			buffer->fPos[0] = x;
			buffer->fPos[1] = y;
			buffer->fPos[2] = z;
			break;
		}
	}
}

void AudioOutput::setBufferPosition(const AudioOutputToken &token, float x, float y, float z) {
	if (!token) {
		return;
	}

	emit bufferPositionChanged(token.m_buffer, x, y, z);
}

void AudioOutput::removeBuffer(AudioOutputBuffer *buffer) {
	if (!buffer) {
		return;
	}

	emit bufferInvalidated(buffer);
}

bool AudioOutput::setHRTF(std::string &newPath) {
	std::shared_ptr< BRTServices::CHRTF > temp_hrtf_loaded = std::make_shared< BRTServices::CHRTF >();
	bool result = sofaReader.ReadHRTFFromSofa(newPath, temp_hrtf_loaded, HRTFRESAMPLINGSTEP, BRTServices::TEXTRAPOLATION_METHOD::nearest_point);

	if (result) {
		envListener->RemoveHRTF();
		result &= envListener->SetHRTF(temp_hrtf_loaded);
		if (!result)
			envListener->SetHRTF(hrtf_loaded);
		else
			hrtf_loaded = temp_hrtf_loaded;
	}
	return result;
}

void AudioOutput::removeUser(const ClientUser *user) {
	removeBuffer(qmOutputs.value(user));
}

void AudioOutput::removeToken(AudioOutputToken &token) {
	removeBuffer(token.m_buffer);
	token = {};
}

AudioOutputToken AudioOutput::playSample(const QString &filename, float volume, bool loop) {
	SoundFile *handle = AudioOutputSample::loadSndfile(filename);
	if (!handle)
		return AudioOutputToken();

	Timer t;
	const quint64 oneSecond = 1000000;

	while (!t.isElapsed(oneSecond) && (iMixerFreq == 0) && isAlive()) {
		QThread::yieldCurrentThread();
	}

	// If we've waited for more than one second, we declare timeout.
	if (t.isElapsed(oneSecond)) {
		qWarning("AudioOutput: playSample() timed out after 1 second: device not ready");
		return AudioOutputToken();
	}

	if (!iMixerFreq)
		return AudioOutputToken();

	QWriteLocker locker(&qrwlOutputs);
	AudioOutputSample *sample = new AudioOutputSample(handle, volume, loop, iMixerFreq, iBufferSize);
	qmOutputs.insert(nullptr, sample);

	return AudioOutputToken(sample);
}

void AudioOutput::initializeMixer(const unsigned int *chanmasks, bool forceheadphone) {
	delete[] fSpeakers;
	delete[] bSpeakerPositional;
	delete[] fSpeakerVolume;
	delete[] fStereoPanningFactor;

	fSpeakers            = new float[iChannels * 3];
	bSpeakerPositional   = new bool[iChannels];
	fSpeakerVolume       = new float[iChannels];
	fStereoPanningFactor = new float[iChannels * 2];

	memset(fSpeakers, 0, sizeof(float) * iChannels * 3);
	memset(bSpeakerPositional, 0, sizeof(bool) * iChannels);

	for (unsigned int i = 0; i < iChannels; ++i)
		fSpeakerVolume[i] = 1.0f;

	if (iChannels > 1) {
		for (unsigned int i = 0; i < iChannels; i++) {
			float *s              = &fSpeakers[3 * i];
			bSpeakerPositional[i] = true;

			switch (chanmasks[i]) {
				case SPEAKER_FRONT_LEFT:
					s[0] = -0.5f;
					s[2] = 1.0f;
					break;
				case SPEAKER_FRONT_RIGHT:
					s[0] = 0.5f;
					s[2] = 1.0f;
					break;
				case SPEAKER_FRONT_CENTER:
					s[2] = 1.0f;
					break;
				case SPEAKER_LOW_FREQUENCY:
					break;
				case SPEAKER_BACK_LEFT:
					s[0] = -0.5f;
					s[2] = -1.0f;
					break;
				case SPEAKER_BACK_RIGHT:
					s[0] = 0.5f;
					s[2] = -1.0f;
					break;
				case SPEAKER_FRONT_LEFT_OF_CENTER:
					s[0] = -0.25;
					s[2] = 1.0f;
					break;
				case SPEAKER_FRONT_RIGHT_OF_CENTER:
					s[0] = 0.25;
					s[2] = 1.0f;
					break;
				case SPEAKER_BACK_CENTER:
					s[2] = -1.0f;
					break;
				case SPEAKER_SIDE_LEFT:
					s[0] = -1.0f;
					break;
				case SPEAKER_SIDE_RIGHT:
					s[0] = 1.0f;
					break;
				case SPEAKER_TOP_CENTER:
					s[1] = 1.0f;
					s[2] = 1.0f;
					break;
				case SPEAKER_TOP_FRONT_LEFT:
					s[0] = -0.5f;
					s[1] = 1.0f;
					s[2] = 1.0f;
					break;
				case SPEAKER_TOP_FRONT_CENTER:
					s[1] = 1.0f;
					s[2] = 1.0f;
					break;
				case SPEAKER_TOP_FRONT_RIGHT:
					s[0] = 0.5f;
					s[1] = 1.0f;
					s[2] = 1.0f;
					break;
				case SPEAKER_TOP_BACK_LEFT:
					s[0] = -0.5f;
					s[1] = 1.0f;
					s[2] = -1.0f;
					break;
				case SPEAKER_TOP_BACK_CENTER:
					s[1] = 1.0f;
					s[2] = -1.0f;
					break;
				case SPEAKER_TOP_BACK_RIGHT:
					s[0] = 0.5f;
					s[1] = 1.0f;
					s[2] = -1.0f;
					break;
				default:
					bSpeakerPositional[i] = false;
					fSpeakerVolume[i]     = 0.0f;
					qWarning("AudioOutput: Unknown speaker %d: %08x", i, chanmasks[i]);
					break;
			}
			if (Global::get().s.bPositionalHeadphone || forceheadphone) {
				s[1] = 0.0f;
				s[2] = 0.0f;
				if (s[0] == 0.0f)
					fSpeakerVolume[i] = 0.0f;
			}
		}
		for (unsigned int i = 0; i < iChannels; i++) {
			float d = sqrtf(fSpeakers[3 * i + 0] * fSpeakers[3 * i + 0] + fSpeakers[3 * i + 1] * fSpeakers[3 * i + 1]
							+ fSpeakers[3 * i + 2] * fSpeakers[3 * i + 2]);
			if (d > 0.0f) {
				fSpeakers[3 * i + 0] /= d;
				fSpeakers[3 * i + 1] /= d;
				fSpeakers[3 * i + 2] /= d;
			}
			float *spf = &fStereoPanningFactor[2 * i];
			spf[0]     = (1.0f - fSpeakers[i * 3 + 0]) / 2.0f;
			spf[1]     = (1.0f + fSpeakers[i * 3 + 0]) / 2.0f;
		}
	} else if (iChannels == 1) {
		fStereoPanningFactor[0] = 0.5;
		fStereoPanningFactor[1] = 0.5;
	}
	iSampleSize =
		static_cast< unsigned int >(iChannels * ((eSampleFormat == SampleFloat) ? sizeof(float) : sizeof(short)));
	qWarning("AudioOutput: Initialized %d channel %d hz mixer", iChannels, iMixerFreq);

	if (Global::get().s.bPositionalAudio && iChannels == 1) {
		Global::get().l->logOrDefer(Log::Warning, tr("Positional audio cannot work with mono output devices!"));
	}
	BRTmutex.lock();
	if (globalParameters.GetSampleRate() != iMixerFreq) {
		globalParameters.SetSampleRate(iMixerFreq);
		globalParameters.SetBufferSize(iFrameSize);

		// for (int i = 0; i < envSourceBuffers.size(); i++) {
		//	envSourceBuffers[i] = CMonoBuffer< float >(iFrameSize);
		// }
		bufferProcessed.left  = CMonoBuffer< float >(iFrameSize);
		bufferProcessed.right = CMonoBuffer< float >(iFrameSize);
		listenerRotationQuat  = { 0, 0, 0, 0 };

		std::string sofa_path = "./3DTI_HRTF_IRC1008_256s_48000Hz.sofa";

		setHRTF(sofa_path);
	} else {
		if (!initialized) {

			// for (int i = 0; i < envSourceBuffers.size(); i++) {
			//	envSourceBuffers[i] = CMonoBuffer< float >(iFrameSize);
			// }
			listenerRotationQuat  = { 0, 0, 0, 0 };

			std::string sofa_path = "./3DTI_HRTF_IRC1008_256s_48000Hz.sofa";

			//setHRTF(sofa_path);

			tempTransform.SetPosition(Common::CVector3(0, 0, 0));
			listener->SetListenerTransform(tempTransform);
			initialized = true;
		} else {

		}
	}
	if (envListener->GetHRTF()->GetFilename() == "") {
		std::string sofa_path = "./3DTI_HRTF_IRC1008_256s_48000Hz.sofa";

		setHRTF(sofa_path);
	}
	BRTmutex.unlock();
	/*} else {

		if (Manual::hrtfPath != "") {
			setHRTF(Manual::hrtfPath.toStdString());
		} else {
			std::string sofa_path = "./3DTI_HRTF_IRC1008_256s_48000Hz.sofa";
			setHRTF(sofa_path);
		}
	}*/

	a = std::vector< std::vector< float > >(3);

	for (std::vector< float > &columns : a)
		columns = std::vector< float >(3);

}

bool AudioOutput::mix(void *outbuff, unsigned int frameCount) {
#ifdef USE_MANUAL_PLUGIN
	positions.clear();
	//instead of clear, fetch poisiton data from plugin?
#endif

	// A list of buffers that have audio to contribute
	QList< AudioOutputBuffer * > qlMix;
	// A list of buffers that no longer have any audio to play and can thus be deleted
	QList< AudioOutputBuffer * > qlDel;

	if (Global::get().s.fVolume < 0.01f) {
		return false;
	}

	BRTmutex.lock();

	const float adjustFactor = std::pow(10.f, -18.f / 20);
	const float mul          = Global::get().s.fVolume;
	const unsigned int nchan = iChannels;
	ServerHandlerPtr sh      = Global::get().sh;
	VoiceRecorderPtr recorder;
	if (sh) {
		recorder = Global::get().sh->recorder;
	}

	qrwlOutputs.lockForRead();

	bool prioritySpeakerActive = false;

	// Get the users that are currently talking (and are thus serving as an audio source)
	QMultiHash< const ClientUser *, AudioOutputBuffer * >::const_iterator it = qmOutputs.constBegin();
	while (it != qmOutputs.constEnd()) {
		AudioOutputBuffer *buffer = it.value();
		if (!buffer->prepareSampleBuffer(frameCount)) {
			qlDel.append(buffer);
		} else {
			AudioOutputSpeech *speech = qobject_cast< AudioOutputSpeech * >(buffer);
			if (speech) {
				//printf("\n %d", it.key()->iId);
#ifdef USE_MANUAL_PLUGIN
				if (!userPos.contains(it.key()->uiSession)) {
					userPos.insert(it.key()->uiSession, { 0, 0.0001, 0 });
					userBuffer.insert(it.key()->uiSession, CMonoBuffer< float >(frameCount));
					
					envManager.BeginSetup();
					envSources.insert(it.key()->uiSession,
									  envManager.CreateSoundSource< BRTSourceModel::CSourceSimpleModel >(
										  (std::string("caller") + std::to_string(it.key()->uiSession))));
					envListener->ConnectSoundSource(envSources[it.key()->uiSession]);
					envManager.EndSetup();
				}
				if (ClientUser::c_qmUsers.contains(it.key()->uiSession)) {
					const ClientUser *user = it.key();
					Manual::spatializeSpeakers(it.key()->uiSession, (float *) userPos[it.key()->uiSession]);
					Position3D newPos = userPos.value(it.key()->uiSession);
					buffer->fPos[0]   = newPos.x;
					buffer->fPos[1]   = newPos.y;
					buffer->fPos[2]   = newPos.z;
					userBuffer[user->uiSession].resize(frameCount);

				}

				//logFile << "\n" << it.key()->qsName.toStdString().c_str();
				//errno_t err;
				//// Reassign "stderr" to "freopen.out":
				//err = freopen_s(&stream, "talkLog.out", "w", stderr);
				//if (err != 0)
				//	fprintf(stdout, "error on freopen\n");
				//else {
				//	fprintf(stream, "\n%s", it.key()->qsName.toStdString().c_str());
				//	fclose(stream);
				//}
#endif
			}
			qlMix.append(buffer);

			const ClientUser *user = it.key();
			if (user && user->bPrioritySpeaker) {
				prioritySpeakerActive = true;
			}
		}
		++it;
	}

#ifdef USE_MANUAL_PLUGIN
	Manual::bufferLock.lock();
	if (Manual::bufferToBeDeleted.size() > 0) {
		for (int i = 0; i < Manual::bufferToBeDeleted.size(); i++) {
			userPos.remove(Manual::bufferToBeDeleted[i]);
			userBuffer.remove(Manual::bufferToBeDeleted[i]);
			if (envSources[Manual::bufferToBeDeleted[i]]) {
				envManager.BeginSetup();
				envListener->DisconnectSoundSource(envSources[Manual::bufferToBeDeleted[i]]);
				envManager.RemoveSoundSource(envSources[Manual::bufferToBeDeleted[i]]->GetID());
				envManager.EndSetup();
				envSources.remove(Manual::bufferToBeDeleted[i]);
			}
		}
		Manual::bufferToBeDeleted.clear();
	}
	if (Manual::hrtfChanged) {
		setHRTF(Manual::hrtfPath.toStdString());
		Manual::hrtfChanged = false;
	}
	if (Manual::isMono && envListener->IsSpatializationEnabled()) {
		envListener->DisableSpatialization();
	} else if (!Manual::isMono && !envListener->IsSpatializationEnabled()) {
		envListener->EnableSpatialization();
	}
	Manual::bufferLock.unlock();
#endif

	if (Global::get().prioritySpeakerActiveOverride) {
		prioritySpeakerActive = true;
	}

	// If the audio backend uses a float-array we can sample and mix the audio sources directly into the output.
	// Otherwise we'll have to use an intermediate buffer which we will convert to an array of shorts later
	static std::vector< float > fOutput;
	fOutput.resize(iChannels * frameCount);
	float *output = (eSampleFormat == SampleFloat) ? reinterpret_cast< float * >(outbuff) : fOutput.data();
	memset(output, 0, sizeof(float) * frameCount * iChannels);

	//////////////////////////////////////////
	// change into rebuffering ////////////////
	//if (envListener->GetHRTF()->GetFilename() == "") {
	//	if (Manual::hrtfPath != "") {
	//		setHRTF(Manual::hrtfPath.toStdString());
	//	} else {
	//		std::string sofa_path = "./3DTI_HRTF_IRC1008_256s_48000Hz.sofa";
	//		setHRTF(sofa_path);
	//	}
	//}

	globalParameters.SetBufferSize(frameCount);

	//for (int i = 0; i < envSourceBuffers.size(); i++) {
	//	envSourceBuffers[i].resize(frameCount);
	//}
	bufferProcessed.left.resize(frameCount);
	bufferProcessed.right.resize(frameCount);
	//////////////////////////////////////////

	if (!qlMix.isEmpty() && !newInstance) {
		// There are audio sources available -> mix those sources together and feed them into the audio backend
		static std::vector< float > speaker;
		speaker.resize(iChannels * 3);
		static std::vector< float > svol;
		svol.resize(iChannels);

		bool validListener = false;

		// Initialize recorder if recording is enabled
		boost::shared_array< float > recbuff;
		if (recorder) {
			recbuff = boost::shared_array< float >(new float[frameCount]);
			memset(recbuff.get(), 0, sizeof(float) * frameCount);
			recorder->prepareBufferAdds();
		}

		for (unsigned int i = 0; i < iChannels; ++i)
			svol[i] = mul * fSpeakerVolume[i];

		if (Global::get().s.bPositionalAudio && (iChannels > 1) && Global::get().pluginManager->fetchPositionalData()) {
			// Calculate the positional audio effects if it is enabled

			Vector3D cameraDir = Global::get().pluginManager->getPositionalData().getCameraDir();

			Vector3D cameraAxis = Global::get().pluginManager->getPositionalData().getCameraAxis();

			// Direction vector is dominant; if it's zero we presume all is zero.

			if (!cameraDir.isZero()) {
				cameraDir.normalize();

				if (!cameraAxis.isZero()) {
					cameraAxis.normalize();
				} else {
					cameraAxis = { 0.0f, 1.0f, 0.0f };
				}

				const float dotproduct = cameraDir.dotProduct(cameraAxis);
				const float error      = std::abs(dotproduct);
				if (error > 0.5f) {
					// Not perpendicular by a large margin. Assume Y up and rotate 90 degrees.

					float azimuth = 0.0f;
					if (cameraDir.x != 0.0f || cameraDir.z != 0.0f) {
						azimuth = atan2f(cameraDir.z, cameraDir.x);
					}

					float inclination = acosf(cameraDir.y) - static_cast< float >(M_PI) / 2.0f;

					cameraAxis.x = sinf(inclination) * cosf(azimuth);
					cameraAxis.y = cosf(inclination);
					cameraAxis.z = sinf(inclination) * sinf(azimuth);
				} else if (error > 0.01f) {
					// Not perpendicular by a small margin. Find the nearest perpendicular vector.
					cameraAxis = cameraAxis - cameraDir * dotproduct;

					// normalize axis again (the orthogonalized vector us guaranteed to be non-zero
					// as the error (dotproduct) was only 0.5 (and not 1 in which case above operation
					// would create the zero-vector).
					cameraAxis.normalize();
				}
			} else {
				cameraDir = { 0.0f, 0.0f, 1.0f };

				cameraAxis = { 0.0f, 1.0f, 0.0f };
			}

			// Calculate right vector as front X top
			Vector3D right = cameraAxis.crossProduct(cameraDir);
			//float q0, q1, q2, q3;
			//q0 = 0.5 * sqrtf(1 + (cameraDir.x + right.y + cameraAxis.z));
			//q1 = (cameraAxis.y - right.z) / (4 * q0);
			//q2 = (cameraDir.z - cameraAxis.x) / (4 * q0);
			//q3 = (right.x - cameraDir.y) / (4 * q0);

			//a[0] = { right.x, right.y, right.z };
			//a[1] = { cameraAxis.x, cameraAxis.y, cameraAxis.z };
			//a[2] = { cameraDir.x, cameraDir.y, cameraDir.z };

			a[0] = { right.x, cameraAxis.x, cameraDir.x };
			a[1] = { right.y, cameraAxis.y, cameraDir.y };
			a[2] = { right.z, cameraAxis.z, cameraDir.z };

			float trace = a[0][0] + a[1][1] + a[2][2];
			if (trace > 0) {
				float s = 0.5f / sqrtf(trace + 1.0f);
				listenerRotationQuat[0] = 0.25f / s;
				listenerRotationQuat[1]  = (a[2][1] - a[1][2]) * s;
				listenerRotationQuat[2] = (a[0][2] - a[2][0]) * s;
				listenerRotationQuat[3]   = (a[1][0] - a[0][1]) * s;
			} else {
				if (a[0][0] > a[1][1] && a[0][0] > a[2][2]) {
					float s = 2.0f * sqrtf(1.0f + a[0][0] - a[1][1] - a[2][2]);
					listenerRotationQuat[0] = (a[2][1] - a[1][2]) / s;
					listenerRotationQuat[1] = 0.25f * s;
					listenerRotationQuat[2] = (a[0][1] + a[1][0]) / s;
					listenerRotationQuat[3] = (a[0][2] + a[2][0]) / s;
				} else if (a[1][1] > a[2][2]) {
					float s = 2.0f * sqrtf(1.0f + a[1][1] - a[0][0] - a[2][2]);
					listenerRotationQuat[0] = (a[0][2] - a[2][0]) / s;
					listenerRotationQuat[1] = (a[0][1] + a[1][0]) / s;
					listenerRotationQuat[2] = 0.25f * s;
					listenerRotationQuat[3] = (a[1][2] + a[2][1]) / s;
				} else {
					float s = 2.0f * sqrtf(1.0f + a[2][2] - a[0][0] - a[1][1]);
					listenerRotationQuat[0] = (a[1][0] - a[0][1]) / s;
					listenerRotationQuat[1] = (a[0][2] + a[2][0]) / s;
					listenerRotationQuat[2] = (a[1][2] + a[2][1]) / s;
					listenerRotationQuat[3] = 0.25f * s;
				}
			}


			/*
						qWarning("Front: %f %f %f", front[0], front[1], front[2]);
						qWarning("Top: %f %f %f", top[0], top[1], top[2]);
						qWarning("Right: %f %f %f", right[0], right[1], right[2]);
			*/
			// Rotate speakers to match orientation
			for (unsigned int i = 0; i < iChannels; ++i) {
				speaker[3 * i + 0] = fSpeakers[3 * i + 0] * right.x + fSpeakers[3 * i + 1] * cameraAxis.x
									 + fSpeakers[3 * i + 2] * cameraDir.x;
				speaker[3 * i + 1] = fSpeakers[3 * i + 0] * right.y + fSpeakers[3 * i + 1] * cameraAxis.y
									 + fSpeakers[3 * i + 2] * cameraDir.y;
				speaker[3 * i + 2] = fSpeakers[3 * i + 0] * right.z + fSpeakers[3 * i + 1] * cameraAxis.z
									 + fSpeakers[3 * i + 2] * cameraDir.z;
			}
			validListener = true;
		}

		int j = 0;
		//for (CMonoBuffer< float > &sourceBuffer : envSourceBuffers)
		//	sourceBuffer.ApplyGain(0);
		int nBuffer = 0;
		for (AudioOutputBuffer *buffer : qlMix) {
			// Iterate through all audio sources and mix them together into the output (or the intermediate array)
			float *RESTRICT pfBuffer = buffer->pfBuffer;
			float volumeAdjustment   = 1;

			// Check if the audio source is a user speaking or a sample playback and apply potential volume
			// adjustments
			AudioOutputSpeech *speech = qobject_cast< AudioOutputSpeech * >(buffer);
			AudioOutputSample *sample = qobject_cast< AudioOutputSample * >(buffer);
			const ClientUser *user    = nullptr;

			if (speech) {
				user = speech->p;

				volumeAdjustment *= user->getLocalVolumeAdjustments();

				if (sh && sh->m_version >= Mumble::Protocol::PROTOBUF_INTRODUCTION_VERSION) {
					// The new protocol supports sending volume adjustments which is used to figure out the correct
					// volume adjustment for listeners on the server. Thus, we only have to apply that here.
					volumeAdjustment *= speech->m_suggestedVolumeAdjustment;
				} else if (user->cChannel
						   && Global::get().channelListenerManager->isListening(Global::get().uiSession,
																				user->cChannel->iId)
						   && (speech->m_audioContext == Mumble::Protocol::AudioContext::LISTEN)) {
					// We are receiving this audio packet only because we are listening to the channel
					// the speaking user is in. Thus we receive the audio via our "listener proxy".
					// Thus we'll apply the volume adjustment for our listener proxy as well
					volumeAdjustment *= Global::get()
											.channelListenerManager
											->getListenerVolumeAdjustment(Global::get().uiSession, user->cChannel->iId)
											.factor;
				}

				if (prioritySpeakerActive) {
					if (user->tsState != Settings::Whispering && !user->bPrioritySpeaker) {
						volumeAdjustment *= adjustFactor;
					}
				}
			} else if (sample) {
				volumeAdjustment *= sample->getVolume();
			}

			// As the events may cause the output PCM to change, the connection has to be direct in any case
			const int channels = (speech && speech->bStereo) ? 2 : 1;
			// If user != nullptr, then the current audio is considered speech
			assert(channels >= 0);
			emit audioSourceFetched(pfBuffer, frameCount, static_cast< unsigned int >(channels), SAMPLE_RATE,
									static_cast< bool >(user), user);

			// If recording is enabled add the current audio source to the recording buffer
			if (recorder) {
				if (speech) {
					if (speech->bStereo) {
						// Mix down stereo to mono. TODO: stereo record support
						// frame: for a stereo stream, the [LR] pair inside ...[LR]LRLRLR.... is a frame
						for (unsigned int i = 0; i < frameCount; ++i) {
							recbuff[i] += (pfBuffer[2 * i] / 2.0f + pfBuffer[2 * i + 1] / 2.0f) * volumeAdjustment;
						}
					} else {
						for (unsigned int i = 0; i < frameCount; ++i) {
							recbuff[i] += pfBuffer[i] * volumeAdjustment;
						}
					}

					if (!recorder->isInMixDownMode()) {
						recorder->addBuffer(speech->p, recbuff, static_cast< int >(frameCount));
						recbuff = boost::shared_array< float >(new float[frameCount]);
						memset(recbuff.get(), 0, sizeof(float) * frameCount);
					}

					// Don't add the local audio to the real output
					if (qobject_cast< RecordUser * >(speech->p)) {
						continue;
					}
				}
			}

			if (((buffer->fPos[0] != 0.0f) || (buffer->fPos[1] != 0.0f) || (buffer->fPos[2] != 0.0f))) {
				// Add position to position map
#ifdef USE_MANUAL_PLUGIN
				if (user) {
					// The coordinates in the plane are actually given by x and z instead of x and y (y is up)
					positions.insert(user->uiSession, { buffer->fPos[0], buffer->fPos[2] });
				}
#endif

				if (speech) {
					unsigned int userId = speech->p->uiSession;
					if (buffer->bStereo) {
						// Linear-panning stereo stream according to the projection of fSpeaker vector on left-right
						// direction.
						// frame: for a stereo stream, the [LR] pair inside ...[LR]LRLRLR.... is a frame
						for (unsigned int i = 0; i < frameCount; ++i)
							//envSourceBuffers[j][i] = (pfBuffer[2 * i] + pfBuffer[2 * i + 1]);
							userBuffer[userId][i] = (pfBuffer[2 * i] + pfBuffer[2 * i + 1]);
					} else {
						for (unsigned int i = 0; i < frameCount; ++i)
							//envSourceBuffers[j][i] = pfBuffer[i]; // change later
							userBuffer[userId][i] = pfBuffer[i];
					}

					tempTransform.SetPosition(Common::CVector3(buffer->fPos[2], -buffer->fPos[0], buffer->fPos[1]));
					envSources[userId]->SetSourceTransform(tempTransform);
					//envSources[j]->SetBuffer(envSourceBuffers[j]);
					envSources[userId]->SetBuffer(userBuffer[userId]);
					j++;


				} else {
					// If positional audio is enabled, calculate the respective audio effect here
					Position3D outputPos = { buffer->fPos[0], buffer->fPos[1], buffer->fPos[2] };
					Position3D ownPos    = Global::get().pluginManager->getPositionalData().getCameraPos();

					Vector3D connectionVec = outputPos - ownPos;
					float len              = connectionVec.norm();

					if (len > 0.0f) {
						// Don't use normalize-func in order to save the re-computation of the vector's length
						connectionVec.x /= len;
						connectionVec.y /= len;
						connectionVec.z /= len;
					}
					/*
									qWarning("Voice pos: %f %f %f", aop->fPos[0], aop->fPos[1], aop->fPos[2]);
									qWarning("Voice dir: %f %f %f", connectionVec.x, connectionVec.y, connectionVec.z);
					*/
					if (!buffer->pfVolume) {
						buffer->pfVolume = new float[nchan];
						for (unsigned int s = 0; s < nchan; ++s)
							buffer->pfVolume[s] = -1.0;
					}

					if (!buffer->piOffset) {
						buffer->piOffset = std::make_unique< unsigned int[] >(nchan);
						for (unsigned int s = 0; s < nchan; ++s) {
							buffer->piOffset[s] = 0;
						}
					}

					const bool isAudible =
						(Global::get().s.fAudioMaxDistVolume > 0) || (len < Global::get().s.fAudioMaxDistance);

					for (unsigned int s = 0; s < nchan; ++s) {
						const float dot = bSpeakerPositional[s] ? connectionVec.x * speaker[s * 3 + 0]
																	  + connectionVec.y * speaker[s * 3 + 1]
																	  + connectionVec.z * speaker[s * 3 + 2]
																: 1.0f;
						float channelVol;
						if (isAudible) {
							// In the current contex, we know that sound reaches at least one ear.
							channelVol = svol[s] * calcGain(dot, len) * volumeAdjustment;
						} else {
							// The user has set the minimum positional volume to 0 and this sound source
							// is exceeding the positional volume range. This means that the sound is completely
							// inaudible at the current position. We therefore set the volume the to 0,
							// making sure the user really can not hear any audio from that source.
							channelVol = 0;
						}

						float *RESTRICT o   = output + s;
						const float old     = (buffer->pfVolume[s] >= 0.0f) ? buffer->pfVolume[s] : channelVol;
						const float inc     = (channelVol - old) / static_cast< float >(frameCount);
						buffer->pfVolume[s] = channelVol;

						// Calculates the ITD offset of the audio data this frame.
						// Interaural Time Delay (ITD) is a small time delay between your ears
						// depending on the sound source position on the horizontal plane and the
						// distance between your ears.
						//
						// Offset for ITD is not applied directly, but rather the offset is interpolated
						// linearly across the entire chunk, between the offset of the last chunk and the
						// newly calculated offset for this chunk. This prevents clicking / buzzing when the
						// audio source or camera is moving, because abruptly changing offsets (and thus
						// abruptly changing the playback position) will create a clicking noise.
						// Normalize dot to range [0,1] instead [-1,1]
						const int offset    = static_cast< int >(INTERAURAL_DELAY * (1.0f + dot) / 2.0f);
						const int oldOffset = static_cast< int >(buffer->piOffset[s]);
						const float incOffset =
							static_cast< float >(offset - oldOffset) / static_cast< float >(frameCount);
						buffer->piOffset[s] = static_cast< unsigned int >(offset);
						/*
											qWarning("%d: Pos %f %f %f : Dot %f Len %f ChannelVol %f", s,
						   speaker[s*3+0], speaker[s*3+1], speaker[s*3+2], dot, len, channelVol);
						*/
						if ((old >= 0.00000001f) || (channelVol >= 0.00000001f)) {
							for (unsigned int i = 0; i < frameCount; ++i) {
								unsigned int currentOffset = static_cast< unsigned int >(
									static_cast< float >(oldOffset) + incOffset * static_cast< float >(i));
								if (speech && speech->bStereo) {
									// Mix stereo user's stream into mono
									// frame: for a stereo stream, the [LR] pair inside ...[LR]LRLRLR.... is a frame
									o[i * nchan] += (pfBuffer[2 * i + currentOffset] / 2.0f
													 + pfBuffer[2 * i + currentOffset + 1] / 2.0f)
													* (old + inc * static_cast< float >(i));
								} else {
									o[i * nchan] += pfBuffer[i + currentOffset] * (old + inc * static_cast< float >(i));
								}
							}
						}
					}
				}
			} else {


				if (speech) {
					unsigned int userId = speech->p->uiSession;
					if (buffer->bStereo) {
						// Linear-panning stereo stream according to the projection of fSpeaker vector on left-right
						// direction.
						// frame: for a stereo stream, the [LR] pair inside ...[LR]LRLRLR.... is a frame
						for (unsigned int i = 0; i < frameCount; ++i)
							userBuffer[userId][i] = (pfBuffer[2 * i] + pfBuffer[2 * i + 1]);
							//envSourceBuffers[j][i] = (pfBuffer[2 * i] + pfBuffer[2 * i + 1]);
					} else {
						for (unsigned int i = 0; i < frameCount; ++i)
							userBuffer[userId][i] = pfBuffer[i]; // change later
							//envSourceBuffers[j][i] = pfBuffer[i]; //change later
					}
					tempTransform.SetPosition(Common::CVector3(0, 0, 0));
					envSources[userId]->SetSourceTransform(tempTransform);
					//envSources[j]->SetBuffer(envSourceBuffers[j]);
					envSources[userId]->SetBuffer(userBuffer[userId]);
					j++;
				
				} else {

					// Mix the current audio source into the output by adding it to the elements of the output buffer
					// after
					// having applied a volume adjustment
					for (unsigned int s = 0; s < nchan; ++s) {
						const float channelVol = svol[s] * volumeAdjustment;
						float *RESTRICT o      = output + s;
						if (buffer->bStereo) {
							// Linear-panning stereo stream according to the projection of fSpeaker vector on left-right
							// direction.
							// frame: for a stereo stream, the [LR] pair inside ...[LR]LRLRLR.... is a frame
							for (unsigned int i = 0; i < frameCount; ++i)
								o[i * nchan] += (pfBuffer[2 * i] * fStereoPanningFactor[2 * s + 0]
												 + pfBuffer[2 * i + 1] * fStereoPanningFactor[2 * s + 1])
												* channelVol;
						} else {
							for (unsigned int i = 0; i < frameCount; ++i)
								o[i * nchan] += pfBuffer[i] * channelVol;
						}
					}
				
				}
			}
			nBuffer++;
		}

		if (validListener) {

			Position3D ownPos = Global::get().pluginManager->getPositionalData().getCameraPos();
			tempTransform.SetPosition(Common::CVector3(ownPos.z, -ownPos.x, ownPos.y));
			tempTransform.SetOrientation(Common::CQuaternion(listenerRotationQuat[0], listenerRotationQuat[3],
															 listenerRotationQuat[1], listenerRotationQuat[2]));
			listener->SetListenerTransform(tempTransform);
			tempTransform.SetOrientation(Common::CQuaternion());

		} else {

			Position3D ownPos = Global::get().pluginManager->getPositionalData().getCameraPos();
			tempTransform.SetPosition(Common::CVector3(ownPos.z, -ownPos.x, ownPos.y));
			listener->SetListenerTransform(tempTransform);
		
		}

		envManager.ProcessAll();
		listener->GetBuffers(bufferProcessed.left, bufferProcessed.right);
		float *RESTRICT o = output;
		if (nchan >= 2) {
			for (unsigned int i = 0; i < frameCount; ++i) {
				o[i * nchan]       += bufferProcessed.left[i];
				o[(i * nchan) + 1] += bufferProcessed.right[i];
			}
		}

		if (recorder && recorder->isInMixDownMode()) {
			recorder->addBuffer(nullptr, recbuff, static_cast< int >(frameCount));
		}
	}

	bool pluginModifiedAudio = false;
	emit audioOutputAboutToPlay(output, frameCount, nchan, SAMPLE_RATE, &pluginModifiedAudio);

	if (pluginModifiedAudio || (!qlMix.isEmpty())) {
		// Clip the output audio
		if (eSampleFormat == SampleFloat)
			for (unsigned int i = 0; i < frameCount * iChannels; i++)
				output[i] = qBound(-1.0f, output[i], 1.0f);
		else
			// Also convert the intermediate float array into an array of shorts before writing it to the outbuff
			for (unsigned int i = 0; i < frameCount * iChannels; i++)
				reinterpret_cast< short * >(outbuff)[i] =
					static_cast< short >(qBound(-32768.f, (output[i] * 32768.f), 32767.f));
	}
	if (newInstance)
		newInstance = false;
	qrwlOutputs.unlock();
	BRTmutex.unlock();
	// Delete all AudioOutputBuffer that no longer provide any new audio
	for (AudioOutputBuffer *buffer : qlDel) {
		removeBuffer(buffer);
	}

#ifdef USE_MANUAL_PLUGIN
	//Manual::setSpeakerPositions(positions);
#endif

	// Return whether data has been written to the outbuff
	return (pluginModifiedAudio || (!qlMix.isEmpty()));
}

bool AudioOutput::isAlive() const {
	return isRunning();
}

unsigned int AudioOutput::getMixerFreq() const {
	return iMixerFreq;
}

void AudioOutput::setBufferSize(unsigned int bufferSize) {
	iBufferSize = bufferSize;
}
