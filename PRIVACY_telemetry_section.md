## NEW SECTION — insert as Section 2 (renumber the rest), or fold into Section 1

### In-Software (Plugin) Data Collection — Optional, Off by Default

The CUE Sampler plugin includes an **optional** data-sharing feature ("Data Sharing") that is **turned OFF by default**. No usage data is collected from or transmitted by the plugin unless you affirmatively enable Data Sharing using the in-plugin toggle. You can turn it off again at any time, which stops all further collection and transmission.

**What is collected when (and only when) you enable Data Sharing:**

- A randomly generated installation identifier (a one-time random ID stored locally that does not contain your name, email, or any account information);
- A **one-way hash** (an irreversible fingerprint) of audio samples you load into the plugin — we cannot reconstruct, listen to, or recover your audio from this value, and we do not collect or transmit your actual audio;
- Technical characteristics of loaded samples: duration, sample rate, and number of channels;
- Tempo (BPM) detection results and any manual corrections you make to them;
- Musical key detection results and any manual corrections you make to them;
- Timestamps for the above events.

**Why we collect it:** solely to measure and improve the accuracy of the plugin's automatic BPM and key detection. This data is technical and product-improvement oriented; it is not used for advertising and is not sold.

**Where it goes:** when enabled, these events are sent securely to our data-collection endpoint hosted on Google's infrastructure (Google Apps Script / Google Workspace), which acts as a service provider/processor on our behalf. We do not share this data with advertisers or other third parties.

**Your control and rights:** Data Sharing is opt-in. Because the data is keyed to a random installation identifier rather than your identity, in most cases we are unable to link it back to you personally. If you have enabled Data Sharing and wish to request deletion of data associated with your installation identifier, contact us at cue@cuesampler.com and provide your installation identifier (viewable in the plugin), and we will delete the associated records where we are able to identify them.
