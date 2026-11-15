<img width="960" height="544" alt="141256" src="https://github.com/user-attachments/assets/166f3bbf-da1a-44c3-9da6-680872ed0430" />
<img width="960" height="544" alt="141258" src="https://github.com/user-attachments/assets/9c571ac7-fca4-4ed9-907a-2dbf9c3e9032" />
<img width="960" height="544" alt="141257" src="https://github.com/user-attachments/assets/5d97c373-c50e-446c-965f-daf1bc50ac29" />
<img width="960" height="544" alt="141261" src="https://github.com/user-attachments/assets/0b9c1614-c8c7-402c-a12d-fd248bd8097b" />
<img width="960" height="544" alt="141260" src="https://github.com/user-attachments/assets/b72d84ca-e9c8-41f8-9b1e-64468788eb88" />
<img width="960" height="544" alt="141259" src="https://github.com/user-attachments/assets/e7cf5c7d-d1dd-469a-b943-f792b8632977" />
<img width="960" height="544" alt="141264" src="https://github.com/user-attachments/assets/700302f7-bbdf-4f2d-bd52-5321f144969f" />
<img width="960" height="544" alt="141263" src="https://github.com/user-attachments/assets/2320fd2e-b49d-43d1-8b23-1c96e685a29e" />
<img width="960" height="544" alt="141262" src="https://github.com/user-attachments/assets/23b3e1f8-6ff0-4c8c-a9ae-3f540186a7e4" />
<img width="960" height="544" alt="141266" src="https://github.com/user-attachments/assets/5b85722e-78f7-4ffb-8e23-ca995477056c" />
<img width="960" height="544" alt="141265" src="https://github.com/user-attachments/assets/42f4b881-451a-4399-a2da-d50709dc05b1" />
<img width="960" height="544" alt="141269" src="https://github.com/user-attachments/assets/075305f7-171f-43e4-89d1-d70798508907" />
<img width="960" height="544" alt="141268" src="https://github.com/user-attachments/assets/3fe4c9c9-790f-4c1f-aae9-253ab3f2acb5" />
<img width="960" height="544" alt="141267" src="https://github.com/user-attachments/assets/01938b1b-fab0-44fb-8e76-8ba974857282" />
# mania vita!

A homebrew osu!mania-style rhythm game for the **PlayStation Vita**, written in C with SDL2.

Play your osu!mania beatmaps (1K to 5K) on the Vita, with skins, mods, video backgrounds, touch controls and local scores.

> **Fan project.** mania vita! is not affiliated with, endorsed by, or connected to osu! or ppy Pty Ltd.
> No osu! code or assets are included. You bring your own beatmaps and skins.

<!-- Screenshots: put images in a "screenshots" folder and uncomment these lines
![Song select](screenshots/select.png)
![Gameplay](screenshots/gameplay.png)
-->

---

## Features

- **1K, 2K, 3K, 4K and 5K** osu!mania maps, with normal and long notes
- **Automatic .osz import**: drop `.osz` files in the songs folder and they're extracted on the next scan
- **Song select** groups difficulties by song, with smooth scrolling, star ratings, sorting, search (Vita keyboard), song previews and background images
- **osu!mania scoring** (ScoreV1, max 1,000,000), MAX/300/200/100/50/MISS judgements, accuracy and grades
- **Timing windows** based on each map's OD
- **12 mods**: Easy, No Fail, Hard Rock, Sudden Death, Perfect, Hidden, Fade In, Flashlight, Mirror, Random, Autoplay, Cinema
- **Health bar** and a Failed screen
- **Hit error bar** with EARLY / LATE indicator
- **Local leaderboards** per difficulty, a Scores screen (view and delete scores), and FC / PFC tags
- **Skins** from `skin.ini` (column widths, hit position, animated notes and keys)
- **6 note colour palettes**
- **Hitsounds**: your own files, sounds from the skin, or the built-in ones
- **Video backgrounds** (MP4 / H.264, hardware decoded)
- **Front touchscreen**: tap lanes in-game and scroll the song list
- **Rear touchpad** can hit the middle lane in 1K, 3K and 5K
- **Button bindings for each key mode**
- **Settings screen with a live playfield preview**

---

## Installing

1. Download `maniavita.vpk` from the [Releases](../../releases) page.
2. Copy it to your Vita and install it with **VitaShell**.
3. Start **mania vita!** once. It creates the data folder:
   `ux0:data/osumania/`

> If you used an older version called "osu!mania", the new one installs over it.
> Your songs, scores and settings stay.

---

## Adding songs

Copy `.osz` files **or** extracted song folders to:

```
ux0:data/osumania/songs/
```

Then press **Start** on song select to rescan. `.osz` files are extracted automatically and the original is deleted. A file that fails to extract is renamed to `.osz.bad`.

Only **osu!mania 1K-5K** maps are loaded. Other modes and key counts are skipped.

---

## Controls

### Song select
| Button | Action |
|---|---|
| Up / Down | Change song |
| Left / Right | Change difficulty |
| L / R | Jump 5 songs |
| ✕ | Play |
| □ | Mods |
| ○ | Sort / Search |
| △ | Settings |
| Select | Scores |
| Start | Rescan songs |

### In-game
| Button | Action |
|---|---|
| Lane buttons | Set for each key mode in **Settings → NK buttons** |
| Front touch | Tap the lanes (if Touch screen is on) |
| Rear touch | Middle lane in 1K / 3K / 5K (if Rear touch is on) |
| Start | Pause |

On the pause menu you can resume, retry, change scroll speed and audio offset, or quit.

---

## Customising

Everything lives in `ux0:data/osumania/`:

| File / folder | What it does |
|---|---|
| `songs/` | Your beatmaps |
| `skins/` | osu! skins (one folder per skin, with `skin.ini`) |
| `hit.wav` / `hit.ogg` | Your own hit sound |
| `miss.wav` / `miss.ogg` | Your own miss / combo break sound |
| `font.ttf` | Replaces the built-in font |
| `settings.txt` | Saved settings (made automatically) |
| `scores.txt` | Your local scores (back this up!) |

Hitsounds are used in this order: your own files, then the skin's `normal-hitnormal` / `combobreak`, then the built-in sounds.

---

## Video backgrounds

The Vita's hardware decoder only plays **MP4 (H.264)**. If a map uses an `.avi` or `.flv` video, the game looks for an `.mp4` with the same name instead.

To convert a video with [ffmpeg](https://ffmpeg.org/):

```
ffmpeg -i video.avi -c:v libx264 -profile:v main -level 3.1 -vf "scale=960:544:force_original_aspect_ratio=decrease,pad=960:544:(ow-iw)/2:(oh-ih)/2" -r 30 -pix_fmt yuv420p -an -movflags +faststart video.mp4
```

Best settings: H.264 Main profile, level 3.1, 960×544 or smaller, 30 fps, no audio track.
Videos can be turned off in Settings.

---

## Building from source

You need [VitaSDK](https://vitasdk.org/) with these libraries (install them with `vdpm`):
SDL2, SDL2_image, SDL2_mixer, SDL2_ttf, zlib

```
git clone https://github.com/sadports102/maniavita.git
cd maniavita
mkdir build
cd build
cmake ..
make
```

This makes `maniavita.vpk` in the `build` folder.

---

## Credits

- Gameplay based on **osu!mania** by ppy. This is an independent fan remake.
- Built with **SDL2** and **VitaSDK**.
- Thanks to the Vita homebrew community.

## License

MIT License
A short and simple permissive license with conditions only requiring preservation of copyright and license notices. Licensed works, modifications, and larger works may be distributed under different terms and without source code.
Permissions
Commercial use
Modification
Distribution
Private use
Limitations
Liability
Warranty
Conditions
License and copyright notice
This is not legal advice. Learn more about repository licenses
