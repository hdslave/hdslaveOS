hdslaveOS {version} for Akai MPC Sample
========================================

hdslaveOS is based on Melodee by Kerem Kiliç (Ellic Studio). It runs from
the microSD card and stores projects, samples, plug-ins and settings in
the card's Melodee folder.

Install or update
-----------------
1. Back up the card and extract the ZIP on your computer. If you have
   the complete installer, open START-HERE.txt and use SD-Card/.
2. For direct boot, back up the card's existing top-level autorun/ and
   autorun.sh. Copy both replacements from the package to the top level.
   For SampleBoot, keep its launcher. Copy the contents of autorun/
   into a new autorun/mods/hdslaveOS <version>/ folder. Check that
   autorun.json is directly inside that version folder.
3. If you downloaded the plug-ins, copy the supplied files into
   Melodee/Plugins/hdslave/ on the card. Keep their filenames so existing
   projects can find them.
4. Eject the card, insert it into the powered-off MPC, and start it.
   Choose Yes at the firmware 1.4 autorun prompt. With SampleBoot, select
   the new hdslaveOS entry.

Use these steps for each unsigned hdslaveOS ZIP. The in-app updater
expects signed Melodee releases.

Firmware
--------
Use MPC Sample firmware 1.3.0.12, 1.4.0.7 or 1.4.0.8. On firmware 1.4,
turn on Developer Mode in the stock app's Settings under DEV
ENHANCEMENTS (BETA). On firmware 1.4, audio runs at 44.1 kHz and Screen
Mirror is unavailable.

On a new install, Stage opens with the track types on track 1. Hold
PAD BANK and press a pad to select an empty track. Choose Plugin to
open the plug-in picker or Drum to create a kit.

Settings > SCREEN > Display refresh caps built-in screen updates at
20, 30 or 60 fps. Mirror frame rate sets the USB camera's output rate.

To return to the stock app, press Power briefly and choose MPC APP.
To boot the stock app directly, power off, remove the card and restart.

Quickstart: {source}/blob/hdslave/docs/QUICKSTART.md
User guide: {source}/blob/hdslave/docs/GUIDE.md
Releases: {source}/releases/latest
Issues: {source}/issues

For a crash report, include the file from Melodee/Crashes and the
version shown in Settings > ABOUT > B3.

Version and source
------------------
hdslaveOS {version}, source commit {commit}
Source code: {source}
sample-base: https://github.com/hdslave/sample-base
ellic-core: https://github.com/hdslave/ellic-core

hdslaveOS is licensed under AGPL-3.0. See licenses/ for the project,
JUCE, the screen fonts, and the separate stem separator and model.
hdslaveOS is not affiliated with or endorsed by inMusic or Akai Professional.
