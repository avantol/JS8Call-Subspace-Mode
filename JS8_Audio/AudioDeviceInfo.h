#ifndef JS8_AUDIO_AUDIODEVICEINFO_H
#define JS8_AUDIO_AUDIODEVICEINFO_H

#include <QByteArray>
#include <QMetaType>
#include <QString>

// Lightweight POD describing an audio endpoint, replacing QAudioDevice
// in JS8Call's audio APIs as we migrate from Qt6Multimedia to miniaudio.
//
// `id` is the hex-encoded miniaudio ma_device_id (a backend-specific
// union, sized 256 bytes). Hex encoding lets the value round-trip
// through QVariant and QSettings without holding a live ma_context,
// and lets the device be identified across app restarts.
struct AudioDeviceInfo
{
    enum Mode { Input, Output };

    QByteArray id;
    QString    description;
    Mode       mode      = Input;
    bool       isDefault = false;

    bool isNull() const { return id.isEmpty(); }

    bool operator==(AudioDeviceInfo const & rhs) const
    {
        return id == rhs.id && mode == rhs.mode;
    }

    bool operator!=(AudioDeviceInfo const & rhs) const { return !(*this == rhs); }
};

Q_DECLARE_METATYPE(AudioDeviceInfo)

#endif // JS8_AUDIO_AUDIODEVICEINFO_H
