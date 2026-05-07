# JS8Call Subspace Edition "Tranya" (3.75 sec Tx period, async protocol)
JS8Call Subspace mode is a continued development of JS8Call that adds a new "Subspace" mode for faster communication, spectral efficiency, and freedom from time accuracy requirements.

Installers are available for Windows, Mac OS, Linux, and Raspberry Pi, at the <a href="https://github.com/avantol/JS8Call-Subspace-Mode/releases/latest">Release page</a>.

JS8Call is an experiment in combining the robustness of FT8 (a weak-signal mode by K1JT) with a messaging and network protocol layer for weak signal communication. The open source software is designed for connecting amateur radio operators who are operating under weak signal conditions and offers real-time keyboard-to-keyboard messaging, store-and-forward messaging, and automatic station announcements.

Now there are 5 modes in JS8Call: Slow, Normal, Fast, Turbo, and **Subspace**.

The cycle time for "Subspace" mode is 3.75 seconds. If that sounds familiar, it's similar in some ways to the transport layer in Decodium FT2, except greatly enhanced for use with JS8Call, where reliability is at the top of the list for message traffic.

Subspace mode eliminates any need to keep your PC's clock adjusted, it's **100% independent of clock time**, do no "drift" issue... ever.
 
Subspace mode is only ~150 Hz wide, compared to the JS8Call-Improved JS8-60 mode requiring ~250 Hz.... this makes a huge difference keeping the audio passband clear of QRM: 17 simultaneous QSOs possible as opposed to 10 in the 2500 Hz audio passband.

<img src="https://github.com/avantol/JS8Call-Subspace-Mode/blob/master/Tranya.JPG"></img>

> Technical sidebar:
> I use carefully selected levels of search depth to "sync up" quickly to the received signal as soon as sound is received, instead of waiting until the message is complete. The process does not depend on clock time at all. Two problems solved!
> 
> There is only a slight loss of sensitivity in signal detection (1 S-unit) compared to JS8Call "Normal" mode, the time saved by our early sync technique is used to produce a signal more likely to be correctly decoded. Another win!
> 
> (Many thanks to Martino IU8LMC for his initial ideas from the L2 async protocol).
> 
> Subspace mode is 5 characters/sec , 4x "Normal mode", good to about -16dB receive SNR.
> 
> Spectral efficiency: 0.162 bits/sec/Hz, compared to 0.091 bits/sec/Hz for JS8Call-Improved's (fastest) JS8-60 mode... 78% better at conserving bandwidth.

An accurate time source is not strictly necessary for Subspace Mode. The typical NTP server has good enough accuracy, as it helps with the standard (slower) modes.

See the [JS8Call Subspace Edition discussion group](https://groups.io/g/JS8Call-Subspace/topics) for full details, and to comment, ask questions, or to suggest something. We look forward to hearing from you!

_I also want to keep JS8Call itself properly maintained aside from adding Subspace mode, things happen all the time that need attention, use "Check for updates" in JS8Call Subspace Edition often._

# Privacy Policy for JS8Call Subspace Edition (Microsoft Store Version)
Effective Date: April 2024

Developer: avantol apps

This application is provided as-is for the Amateur Radio community. 

Privacy is a priority, and this policy describes how the application handles data.

1. Personal Information Collection
This application does not collect, store, or transmit any personal user information, names, email addresses, or location data.

2. Use of Device Capabilities
To function as an Amateur Radio communication tool, the app requires the following permissions:
Microphone (Audio Input): Used strictly for the local decoding of radio signals from your soundcard or radio interface. Audio is processed in real-time and is never recorded, saved, or transmitted over the internet.
Serial Communication: Used to communicate with your radio hardware (CAT control) via COM ports. This data stays entirely local to your machine.
Internet Access: Used solely for optional features such as time synchronization (NTP) or reporting station "spots" to community networks (e.g., PSKReporter), if enabled by the user.

3. Third-Party Sharing
No data is ever shared with the developer or any third-party entities.

4. Data Storage
All logs (ADIF files) and configuration settings are stored locally on your machine in the application's private data folder. These remain under the user's total control.

## JS8Call-improved (original README)
JS8Call-improved is continued development of the original JS8Call project. Most of the same developers have worked on both projects.

Full information on JS8Call, including the User Guide and code documentation is available on our [website](https://js8call-improved.github.io)

Like JS8Call, JS8Call-improved is licensed under the GPLv3, the Qt libraries used by JS8Call-improved are licensed under the LGPL. See [LICENSE](LICENSE) for the details. All copyrights remain with the original holders. Source code for JS8Call-improved is [here](https://github.com/JS8Call-improved/JS8Call-improved)

You can view or save an offline copy of the JS8Call User Guide [here](https://js8call-improved.github.io/JS8Call-improved/d6/d14/md_docs_2user__guide_2JS8Call__User__Guide.html)

### Building JS8Call-improved From Sourcecode
Instructions can be found in [docs](docs) in the source tree for building JS8Call on MacOS, Linux and Windows, as well as a contributor's guide.

### Debugging JS8Call-improved
You can get a lot of debug output reporting what the program does by setting the environment variable `QT_LOGGING_RULES` to an appropriate value.

The value `*.js8=true` gives you all output there is.

Each line starts with a "logging category". These can be used to filter, so fewer lines you are not interested in will be given.

For example, if you are interested in lines starting with `mainwindow.js8` and `configuration.js8` only, to debug a certain problem, set `QT_LOGGING_RULES` to `mainwindow.js8=true;configuration.js8=true` to just see what those two have to say.

You find `decoder.js8` a bit too noisy, you may use the value `*.js8=true;decoder.js8=false` to silence it, but still see all the others.

The official documentation of what we are using here can be found at [https://doc.qt.io/qt-6/qloggingcategory.html#checking-category-configuration](https://doc.qt.io/qt-6/qloggingcategory.html#checking-category-configuration).

### History - JS8Call
JS8Call is an experiment in combining the robustness of FT8 (a weak-signal mode by K1JT) with a messaging and network protocol layer for weak signal communication. The open source software is designed for connecting amateur radio operators who are operating under weak signal conditions and offers real-time keyboard-to-keyboard messaging, store-and-forward messaging, and automatic station announcements.

### Notice
JS8Call is a derivative of the WSJT-X application, restructured and redesigned for message passing using a custom FSK modulation called JS8. It is not supported by nor endorsed by the WSJT-X development group. While the WSJT-X group maintains copyright over the original work and code, JS8Call is a derivative work licensed under and in accordance with the terms of the GPLv3 license. The source code modifications are public and can be found in this repository: https://github.com/js8call/js8call .

* July 6, 2017 - The initial idea of using a modification to the FT8 protocol to support long-form QSOs was developed by Jordan, KN4CRD, and submitted to the WSJT-X mailing list: https://sourceforge.net/p/wsjt/mailman/message/35931540/
* August 31, 2017 - Jordan, KN4CRD, did a little development and modified WSJT-X to support long-form QSOs using the existing FT8 protocol: https://sourceforge.net/p/wsjt/mailman/message/36020051/  He sent a video example to the WSJT-X group: https://widefido.wistia.com/medias/7bb1uq62ga
* January 8, 2018 - Jordan, KN4CRD, started working on the design of a long-form QSO application built on top of FT8 with a redesigned interface.
* February 9, 2018 - Jordan, KN4CRD, submitted question to the WSJT-X group to see if there was any interest in pursuing the idea: https://sourceforge.net/p/wsjt/mailman/message/36221549/
* February 10, 2018 - Jordan KN4CRD, Julian OH8STN, John N0JDS, and the Portable Digital QRP group did an experiment using FSQ. The idea of JS8Call, combining FT8, long-form QSOs, and FSQCall like features was born.
* February 11, 2018 - Jordan, KN4CRD, inquired about the idea of integrating long-form messages into WSJT-X: https://sourceforge.net/p/wsjt/mailman/message/36223372/
* February 12, 2018 - Joe Taylor, K1JT, wrote back: https://sourceforge.net/p/wsjt/mailman/message/36224507/ saying that “Please don't let my comment discourage you from proceeding as you wish, toward something new.”
* March 4, 2018 - Jordan, KN4CRD, published a design document for JS8Call: https://github.com/jsherer/js8call
* July 6, 2018 - Version 0.0.1 of JS8Call released to the development group
* July 15, 2018 - Version 0.1 released - a dozen testers
* July 21, 2018 - Version 0.2 released - 75 testers
* July 27, 2018 - Version 0.3 released - 150 testers
* August 12, 2018 - Version 0.4 released - (“leaked” on QRZ) - 500 testers
* September 2, 2018 - Version 0.5 released - 3000 testers
* September 14, 2018 - Version 0.6 released - 5000 testers
* October 8, 2018 - Version 0.7 released - 6000 testers, name changed to JS8 & JS8Call
* October 31, 2018 - Version 0.8 released - ~7000 testers
* November 15, 2018 - Version 0.9 released - ~7500 testers
* November 30, 2018 - Version 0.10 released - ~7800 testers
* December 18, 2018 - Version 0.11 released - ~8200 testers
* January 1, 2019 - Version 0.12 released - ~9000 testers
* January 23, 2019 - Version 0.13 released - ~9250 testers
* February 7, 2019 - Version 0.14 released - ~9600 testers
* February 21, 2019 - Version 1.0.0-RC1 released - ~10000 testers
* March 11, 2019 - Version 1.0.0-RC2 released - >10000 testers
* March 26, 2019 - Version 1.0.0-RC3 released - >11000 testers
* April 1, 2019 - Version 1.0.0 general availability - Public Release!
* June 6, 2019 - Version 1.1.0 general availability
* November 29, 2019 - Version 2.0.0 general availability - Fast and Turbo speeds introduced!
* December 22, 2019 - Version 2.1.0 general availability - Slow speed introduced!

### JS8Call Updates to v2.3.x
This is, in the flavor of `WSJTX-improved`, an 'improved' version of the original JS8Call, the source
code for which is now hosted on this repository.

I am not the original author, and have no desire to create a fork, add new features, etc. My motivation
was to have a native version of JS8Call that would run on my Apple silicon Mac, using a current version
of the Qt and Hamlib libraries. Along the way, I discovered and corrected a few bugs, and made some minor
visual improvements to the UI.

Anyway.....that's what this does; that's all this does. It's not intended to be anything but a vehicle
by which to provide my changes to the original author.

### Notable Changes
- Use of Fortran has been eliminated; everything that was previously implemented in Fortran has
  been ported to C++.
- The requirement for a separate decoder process and use of shared memory has been eliminated.
- Ported to Qt6, which changed the audio classes in a major way. Fortunately the wsjtx-improved
  team had been down this road already, and had dealt with most of the changes needed to the
  audio stuff.
- Vestiges of the original WSJTX codebase that were are longer relevant have been removed.
- The variable decode depth settings have been removed, as testing demonstrated that decodes
  beyond a depth of 2 were largely hope and dreams. The implementation now decodes at a fixed
  depth of 2 in all cases.
- Did a bit of work with alignment of data in the tables for better presentation.
- Improved the performance and appearance of the audio input VU meter.
- The attenuation slider was designed to look like an audio fader control, and it does a
  decent job of this in the `windows` style. However, the underlying `QSlider` control is not
  great in terms of styling consistency; it looks ok but not great in the `fusion` style, and
  quite bizarre in the `macos` style. I've attempted to rectify this via implementation of a
  custom-drawn `QSlider` implementation that consistently looks like a fader on any platform
  style, with the added advantage of always displaying the dB attenuation value.
- Adapted the waterfall scale drawing methodology to accommodate high-DPI displays.
- Hovering on the waterfall display now shows the frequency as a tooltip.
- The waterfall spectrum display has been substantially improved. This does mean that you'll
  have to re-select your preferred spectrum choice on first use, if your choice wasn't the
  default of 'Cumulative'. 'Linear Average' with a smoothing factor of 3 is particularly
  useful; either is in general a more helpful choice than the raw data shown by 'Current'.
- The waterfall display for Cumulative was displaying raw power, uncorrected to dB. Fixed.
- The waterfall display will now intelligently redraw on resize, rather than clearing.
- The 200Hz WSPR portion of the 30m band is now displayed more clearly, i.e., we label it
  as `WSPR`, and the sub-band indicator is located in a manner consistent with that of the
  JS8 sub-band indicators.
- Converted the boost library to an out-of-tree build.
- Updated the sqlite library.
- Updated the CRCpp library.
- Added the Eigen library.
- Fixed an issue where the message server and APRS client should have been moved to the network
  thread, but because they had parent objects, the moves failed.
- Ported the updated PSK reporter from the upstream WSJTX code, which allows for use of a TCP
  connection, and implements all of the advances in the upstream code, i.e., more efficient
  spotting to PSK Reporter, omission of redundant spots, and posting of spots is now spread
  more widely in time. As with WSJTX, temporarily, in support of the HamSCI Festivals of Eclipse
  Ionospheric Science, spots will be transmitted more frequently during solar eclipses; see
  https://www.hamsci.org/eclipse for details.
- Incorporated revised audio device selection methodology from the upstream WSJTX implementation:
  1. Where possible audio devices that disappear are not forgotten until the user selects
     another device, this should allow temporarily missing devices or forgetting to switch
     on devices before starting JS8Call to be handled more cleanly.
  2. Enumerating  audio devices is expensive and on Linux may take many seconds per device.
     To avoid lengthy blocking behavior until it is absolutely necessary, audio devices are
     not enumerated until one of the "Settings->Audio" device drop-down lists is opened.
     Elsewhere when devices must be discovered the enumeration stops as soon as the configured
     device is  discovered. A status bar message is posted when audio devices are being enumerated
     as a reminder that the UI may block while this is happening.
- Status messages couldn't be displayed in the status bar due to the progress widget taking up
  all available space; for the moment at least, it's restricted to be a defined size.
- Corrected a display resizing issue in the topmost section; seems to have affected only Linux
  systems, but in theory was broken on any platform.
- Updated the UDP reporting API to be multicast-aware.
- Separated display of distance and azimuth in the Calls table.
- Hovering over an azimuth in the Calls table will now display the closest cardinal compass direction.
- Azimuth and distance calculations will now use the 4th Maidenhead pair, i.e., the Extended field,
  if present.
- The Configuration dialog would allow invalid grid squares to be input; it will now allow only a
  valid square.
- Removed the undocumented and hidden `Audio/DisableInputResampling=true` configuration option.
- Windows, and only Windows, required a workaround to the Modulator as a result of changes in
  Qt 6.4, which presented as no sound being generated; OSX and Linux worked fine. The issue is
  described in https://bugreports.qt.io/browse/QTBUG-108672, and the workaround seems like a
  grody hack, but it's what WSJTX uses for the same issue, so we're in fine company here.

Qt6 by default will display using a platform-specific style. As a result, there will be some minor
display inconsistencies, e.g., progress bars, as displayed in the bottom of the main window, are
particularly platform-specific.

The earliest version of OSX that Qt6 supports is 11.0. It's set up to compile and link to run
on 11.0 or later, but I've only tested it on 14.6, 14.7, and 15.3.

Testing on Linux and Windows has been ably provided by Joe Counsil, K0OG, who does the bulk of the
grunt work while I largely just type things and drink coffee.

Allan Bazinet, W6BAZ

### JS8Call v2.4.x and later
All versions of JS8Call up to version 2.3.1 were named "jscall" (lower case). JS8Call-improved was founded by Chris AC9KH to move development forward from v2.3.1. Version 2.4.0 was released as JS8Call-improved on Nov 3, 2025 when development moved to the new team environment. Jordan archived the original [js8call repository](https://github.com/js8call/js8call), joined the JS8Call-improved team and the software was named JS8Call (both upper and lower case) for versions 2.5.0 and later. JS8Call-improved is now the official source of the software. Builds for Mac, Windows and Linux are provided in our [releases](https://github.com/JS8Call-improved/JS8Call-improved/releases) from version 2.4.0 and later. Included in the releases are build assets that you can use to build JS8Call from source code, such as pre-built libraries for MacOS and automated scripts for Linux-based systems.

Chris Olson, AC9KH
