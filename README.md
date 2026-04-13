# JS8Call Subspace Edition "Tranya" (3.75 sec Tx period, async protocol)
JS8Call Subspace mode is a continued development of JS8Call that adds a new "Subspace" mode for faster communication, spectral efficiency, and freedom from time accuracy requirements.

Installers are available for Windows, Mac OS, Linux, and Raspberry Pi, at the <a href="https://github.com/avantol/JS8Call-Subspace-Mode/releases/latest">Release page</a>.

JS8Call is an experiment in combining the robustness of FT8 (a weak-signal mode by K1JT) with a messaging and network protocol layer for weak signal communication. The open source software is designed for connecting amateur radio operators who are operating under weak signal conditions and offers real-time keyboard-to-keyboard messaging, store-and-forward messaging, and automatic station announcements.

Now there are 5 modes in JS8Call: Slow, Normal, Fast, Turbo, and **Subspace**.

The cycle time for "Subspace" mode is 3.75 seconds. If that sounds familiar, it's similar in some ways to the transport layer in Decodium FT2, except greatly enhanced for use with JS8Call, where reliability is at the top of the list for message traffic.

Subspace mode eliminates any need to keep your PC's clock adjusted, it's **100% independent of clock time**, do no "drift" issue... ever.
 
Subspace mode is only ~150 Hz wide, compared to the JS8Call-Improved JS8-60 mode requiring ~250 Hz.... this makes a huge difference keeping the audio passband clear of QRM: 17 simultaneous QSOs possible as opposed to 10 in the 2500 Hz audio passband.

"I hope you will relish it as much as I"
<br><img src="https://github.com/avantol/JS8Call-Subspace-Mode/blob/master/Tranya.JPG"></img>

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

Experiment with "Subspace" mode and please report back how it works for you, Contact me at the [JS8Call Subspace Edition discussion group](https://groups.io/g/JS8Call-Subspace/topics) .

To make finding Subspace messages easy:
- Enable "Mode speed" in the left band activity list (set priority to "Mode speed, fastest first").
- Enable "Mode speed" in the right call sign list.
- Set audio notifications to announce Subspace-only messages.

See the [JS8Call Subspace Edition discussion group](https://groups.io/g/JS8Call-Subspace/topics) for full details, and to comment, ask questions, or to suggest something. We look forward to hearing from you!

_I also want to keep JS8Call itself properly maintained aside from adding Subspace mode, things happen all the time that need attention, use "Check for updates" in JS8Call Subspace Edition often._
