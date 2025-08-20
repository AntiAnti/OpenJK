# OpenJK

This is my attempt to improve quality of cinematics in JA SP. Two formats are supported now: original RoQ and new OGV (theora). Resolution of RoQ video was increased from 512x512 to 1024x1024 and 2048x2048. OGV support isn't ready yet but it works. To improve performance, I process frames decoded by theora on GPU, but this is only possible with rend2 (in console "cl_renderer rdsp-rend2;vid_restart").

> [!NOTE]
> Some technical info. For a video with frame rate = 30 we have 33 ms frame budget (usually rounded to 30). On my PC for 1920x1080 video: frame decoding takes ~5 ms, but for heavy frames it may go to 10-11 ms. We also need to convert YUV420 data to RGBA (add 4-5 ms on x86 CPU with SSE2) and 4-5 ms to blit this 16:9 image to square (2048x2048) texture. As you can see, I'm in safe with 20 ms at max, but user with weaker CPU can lose "heavy" frames. Fortunately, everything after frame decoding can be processed on GPU.
>
> Conclusion: you're completely safe with vanilla renderer and 720p video. For 1080p it's not strictly necessary, but better to use rend2. 2K video also could work fine with rend2, but I limited width of the buffer to 2048px. For absolutely no reason, I'm just greedy for VRAM.

OGV libraries (**ogg** itself, **theora** video decoder and **vorbis** audio decoder) are included as bundled projects and (hopefully) should compile for Linux. There is UseInternalOGGVideo flag in the CMakeLists. Disable it to build the project without OGV support (but you still get 1k and 2k RoQ).

Regardless of the specified extension of the video file name (if any specified), ogv has priority over roq. For example, if the script tried to read "video/ja02" or "video/ja02.roq", at first cl_cin.cpp would look for "video/jk02.ogv". To convert mp4-h264 video to OGV, use *ffmpeg2theora* or *Theora Converter .NET* (GUI for ffmpeg2theora). Keep in mind the game expects sample rate of audio in cinematics = 22050 Hz. I don't recommend to try 2K and 4K videos or frame rate higher than 30, since theora is purely software decoder.

Active branch is rend2-unified-wip.

## Original Overview

OpenJK is a community effort to maintain and improve the game and engine powering Jedi Academy and Jedi Outcast, while maintaining _full backwards compatibility_ with the existing games and mods.  
This project does not intend to add major features, rebalance, or otherwise modify core gameplay.

Our aims are to:

- Improve the stability of the engine by fixing bugs and improving performance.
- Support more hardware (x86_64, Arm, Apple Silicon) and software platforms (Linux, macOS)
- Provide a clean base from which new code modifications can be made.

[![discord](https://img.shields.io/badge/discord-join-7289DA.svg?logo=discord&longCache=true&style=flat)](https://discord.gg/dPNCfeQ)
[![forum](https://img.shields.io/badge/forum-JKHub.org%20OpenJK-brightgreen.svg)](https://jkhub.org/forums/forum/49-openjk/)

[![build](https://github.com/JACoders/OpenJK/actions/workflows/build.yml/badge.svg?branch=master)](https://github.com/JACoders/OpenJK/actions/workflows/build.yml?query=branch%3Amaster)
[![coverity](https://scan.coverity.com/projects/1153/badge.svg)](https://scan.coverity.com/projects/1153)

## Supported Games

| Game | Single Player | Multi Player |
| - | - | - |
| Jedi Academy | ✅ Stable | ✅ Stable |
| Jedi Outcast | 😧 Works, needs attention | 🙅 Not supported - consider [JK2MV](https://jk2mv.org) |

Please direct support queries, discussions and feature requests to the JKHub sub-forum or Discord linked above.

## License

OpenJK is licensed under GPLv2 as free software. You are free to use, modify and redistribute OpenJK following the terms in [LICENSE.txt](https://github.com/JACoders/OpenJK/blob/master/LICENSE.txt)

## For players

To install OpenJK, you will first need Jedi Academy installed. If you don't already own the game you can buy it from online stores such as [Steam](https://store.steampowered.com/app/6020/), [Amazon](https://www.amazon.com/Star-Wars-Jedi-Knight-Academy-Pc/dp/B0000A2MCN) or [GOG](https://www.gog.com/game/star_wars_jedi_knight_jedi_academy).

Download the [latest build](https://github.com/JACoders/OpenJK/releases/tag/latest) ([alt link](https://builds.openjk.org)) for your operating system.

Installing and running OpenJK:

1. Extract the contents of the file into the Jedi Academy `GameData/` folder. For Steam users, this will be in `<Steam Folder>/steamapps/common/Jedi Academy/GameData/`.
1. Run `openjk.x86.exe` (Windows), `openjk.i386` (Linux 32-bit), `openjk.x86_64` (Linux 64-bit) or the `OpenJK` app bundle (macOS), depending on your operating system.

### Linux Instructions

If you do not have an existing JKA installation and need to download the base game:

1. Download and Install SteamCMD [SteamCMD](https://developer.valvesoftware.com/wiki/SteamCMD#Linux).
1. Set the download path using steamCMD: `force_install_dir /path/to/install/jka/`
1. Using SteamCMD Set the platform to windows to download any windows game on steam. @sSteamCmdForcePlatformType "windows"
1. Using SteamCMD download the game, `app_update 6020`.

Extract the contents of the file into the Jedi Academy `GameData/` folder. For Steam users, this will be in `<Steam Folder>/steamapps/common/Jedi Academy/GameData/`.

### macOS Instructions

If you have the Mac App Store Version of Jedi Academy, follow these steps to get OpenJK runnning under macOS:

1. Install [Homebrew](https://brew.sh/) if you don't have it.
1. Open the Terminal app, and enter the command `brew install sdl2`.
1. Extract the contents of the OpenJK DMG into the game directory `/Applications/Star Wars Jedi Knight: Jedi Academy.app/Contents/`
1. Run `OpenJK.app` or `OpenJK SP.app`
1. Savegames, Config Files and Log Files are stored in `/Users/$USER/Library/Application Support/OpenJK/`

## For Developers

### Building OpenJK

- [Compilation guide](https://github.com/JACoders/OpenJK/wiki/Compilation-guide)
- [Debugging guide](https://github.com/JACoders/OpenJK/wiki/Debugging)

### Contributing to OpenJK

- [Fork](https://github.com/JACoders/OpenJK/fork) the project on GitHub
- Create a new branch and make your changes
- Send a [pull request](https://help.github.com/articles/creating-a-pull-request) to upstream (JACoders/OpenJK)

### Using OpenJK as a base for a new mod

- [Fork](https://github.com/JACoders/OpenJK/fork) the project on GitHub
- Change the `GAMEVERSION` define in [codemp/game/g_local.h](https://github.com/JACoders/OpenJK/blob/master/codemp/game/g_local.h) from "OpenJK" to your project name
- If you make a nice change, please consider back-porting to upstream via pull request as described above. This is so everyone benefits without having to reinvent the wheel for every project.

## Maintainers (full list: [@JACoders](https://github.com/orgs/JACoders/people))

Leads:

- [Ensiform](https://github.com/ensiform)
- [razor](https://github.com/Razish)
- [Xycaleth](https://github.com/xycaleth)

## Significant contributors ([full list](https://github.com/JACoders/OpenJK/graphs/contributors))

- [bibendovsky](https://github.com/bibendovsky) (save games, platform support)
- [BobaFett](https://github.com/Lrns123)
- [BSzili](https://github.com/BSzili) (JK2, platform support)
- [Cat](https://github.com/deepy) (infra)
- [Didz](https://github.com/dionrhys)
- [eezstreet](https://github.com/eezstreet)
- exidl (SDL2, platform support)
- [ImperatorPrime](https://github.com/ImperatorPrime) (JK2)
- [mrwonko](https://github.com/mrwonko)
- [redsaurus](https://github.com/redsaurus)
- [Scooper](https://github.com/xScooper)
- [Sil](https://github.com/TheSil)
- [smcv](https://github.com/smcv) (debian packaging)
- [Tristamus](https://tristamus.com>) (icon)
