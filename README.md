# MirrorTV — AirPlay screen mirroring receiver for Google TV / Android TV

MirrorTV turns an Android TV / Google TV device (e.g. a projector) into an AirPlay
receiver, so an iPhone or iPad can mirror its screen to it with **Control Center →
Screen Mirroring**. It's free, has no subscription, ads or time limit, and doesn't add a watermark.

It's built on the open-source [UxPlay](https://github.com/FDH2/UxPlay) AirPlay
implementation. The Android-specific parts are original:

| Part | How it works on Android |
|---|---|
| Discovery (the name in the iPhone's list) | `NsdManager` (Android's built-in mDNS) |
| Video | H.264, hardware decoded with `AMediaCodec` straight onto a `SurfaceView` |
| Mirroring audio | AAC-ELD decoded with `AMediaCodec`, played via AAudio |
| Music-only AirPlay audio | ALAC via Apple's open-source decoder, played via AAudio |

## Get the APK (built by GitHub, nothing to install on your PC)

1. Create a new **empty** repository on github.com (e.g. `mirrortv`, private is fine).
2. In **Git Bash**, inside this folder:
   ```bash
   git remote add origin https://github.com/<your-user>/mirrortv.git
   git push -u origin main
   ```
3. On GitHub open **Actions → Build APK**. The first build takes about 10–15 minutes
   because OpenSSL is compiled. Later builds take about 3–4 minutes.
4. Open the finished run and download the **MirrorTV-apk** artifact (a zip containing `MirrorTV.apk`).

## Install it on the projector (sideload over Wi-Fi)

1. On the projector: **Settings → System → About**, select **Android TV OS build** seven
   times until it says you are a developer.
2. **Settings → System → Developer options** → turn on **USB debugging**. On many Google TV
   devices this also enables network debugging. Note the projector's IP address from
   **Settings → Network & Internet**, or from the MirrorTV start screen later.
3. On Windows, download **SDK Platform-Tools** from Google
   (developer.android.com/tools/releases/platform-tools) and unzip it, e.g. to `C:\platform-tools`.
4. In Git Bash:
   ```bash
   cd /c/platform-tools
   ./adb connect 192.168.1.50:5555      # your projector's IP
   # accept the "Allow debugging?" prompt on the projector (tick "Always allow")
   ./adb install -r ~/Downloads/MirrorTV.apk
   ```
5. MirrorTV now appears in the projector's app row.

To update later, run the same `adb install -r` command with a new APK. Every build is signed with the same key, so updates install over the old version.

## Use

1. Open **MirrorTV** on the projector. It shows its name ("Projector" by default).
2. iPhone: **Control Center → Screen Mirroring → Projector**.
3. Press **Back** on the remote to stop mirroring. The receiver stops when you leave the app.

The start screen lets you **rename** the receiver and switch between **30 and 60 fps**.
30 fps is the default because it's easier on low-end projector chips.

## Troubleshooting

* **Projector not in the iPhone's list:** both devices must be on the same Wi-Fi network.
  Some routers block mDNS/multicast between 2.4 GHz and 5 GHz bands or on "guest" networks.
  Leaving and reopening the app re-announces it.
* **Connects but stays black / stutters:** set 30 fps, and keep the projector on 5 GHz Wi-Fi or Ethernet.
* **Logs** (very useful for fixing problems):
  ```bash
  ./adb logcat -s MirrorTV MirrorTV-raop MirrorTV-video MirrorTV-audio MirrorTV-mdns
  ```

## Build locally (optional)

Requires the Android SDK, NDK 27.2.12479018 and CMake 3.22.1 (e.g. through Android Studio), and Git Bash or Linux/macOS:

```bash
scripts/fetch-deps.sh
ANDROID_NDK_ROOT=<sdk>/ndk/27.2.12479018 scripts/build-openssl.sh
./gradlew assembleRelease
```

## Limitations

* H.264 only (no 4K/H.265 mirroring), and no "AirPlay video" URL hand-off. Apps like
  YouTube fall back to mirroring the screen.
* Video is shown as soon as it is decoded (lowest latency). Audio and video are not
  re-timed against each other, so lip-sync can drift slightly on slow devices.
* Apple doesn't publish the AirPlay protocol. UxPlay's implementation is reverse engineered,
  so a future iOS update could break it until UxPlay is updated. In that case, bump the
  commit in `scripts/fetch-deps.sh`.

## License

GPL-3.0-or-later, the same as UxPlay. Third-party code: UxPlay (GPL-3.0), libplist (LGPL-2.1),
Apple ALAC decoder (Apache-2.0), OpenSSL (Apache-2.0). These are downloaded at build time.
