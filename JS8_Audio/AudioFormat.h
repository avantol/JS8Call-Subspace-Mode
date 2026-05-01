#ifndef JS8_AUDIO_AUDIOFORMAT_H
#define JS8_AUDIO_AUDIOFORMAT_H

#include <QtGlobal>

// Lightweight POD describing PCM stream parameters, replacing
// QAudioFormat in JS8Call's audio APIs. JS8Call only ever uses
// 48 kHz Int16; the struct keeps the seam stable rather than
// modeling a full format space.
struct AudioFormat
{
    enum Sample { Int16 };

    int    sampleRate   = 48000;
    int    channelCount = 1;
    Sample sample       = Int16;

    qint64 bytesForFrames(qint64 frames) const
    {
        return frames * channelCount * sizeof(qint16);
    }

    qint64 framesForBytes(qint64 bytes) const
    {
        return bytes / channelCount / sizeof(qint16);
    }

    bool operator==(AudioFormat const & rhs) const
    {
        return sampleRate == rhs.sampleRate
            && channelCount == rhs.channelCount
            && sample == rhs.sample;
    }

    bool operator!=(AudioFormat const & rhs) const { return !(*this == rhs); }
};

#endif // JS8_AUDIO_AUDIOFORMAT_H
