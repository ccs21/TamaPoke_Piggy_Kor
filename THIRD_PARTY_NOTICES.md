# Third-party notices

This file summarizes third-party software and font material used by the public
source, the asset-free base firmware, or the Windows flasher. Each component
remains subject to its own license. Links identify the corresponding source and
license; full license copies shipped with this repository are in `licenses/`.

## Original TamaPoke source

- Source: <https://github.com/socquique/TamaPoke>
- Copyright: 2026 Quique Tortosa
- License: MIT; the retained notice is in `LICENSE`.

## Noto Sans KR

- Source: <https://github.com/notofonts/noto-cjk>
- Copyright notice: Copyright 2014-2021 Adobe, with Reserved Font Name
  `Source`
- License: SIL Open Font License 1.1
- Full text: `licenses/NotoSansKR-OFL.txt`

The firmware contains a generated Korean bitmap subset derived from this font.
The generated data header identifies that origin.

## Arduino ESP32 Core

- Source: <https://github.com/espressif/arduino-esp32/tree/3.3.11>
- License: GNU Lesser General Public License 2.1 and component-specific notices
- Full project license: `licenses/Arduino-ESP32-LICENSE.md`

The complete Korean-edition firmware source and build script are provided in
this repository so recipients can rebuild the firmware with the identified
Arduino core version.

## Arduino GFX Library

Copyright (c) 2012 Adafruit Industries. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

- Redistributions of source code must retain the above copyright notice,
  this list of conditions and the following disclaimer.
- Redistributions in binary form must reproduce the above copyright notice,
  this list of conditions and the following disclaimer in the documentation
  and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
POSSIBILITY OF SUCH DAMAGE.

Project source: <https://github.com/moononournation/Arduino_GFX>

## SensorLib and XPowersLib

Copyright (c) 2022 Lewis He

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

- SensorLib: <https://github.com/lewisxhe/SensorLib>
- XPowersLib: <https://github.com/lewisxhe/XPowersLib>

## NAudio

Copyright 2020 Mark Heath

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

Project source: <https://github.com/naudio/NAudio>

## Microsoft .NET

The Windows flasher is published as a self-contained .NET application.
Microsoft's .NET license and third-party notices are copied beside the release
executable by `Build-Public.ps1` as `DOTNET-LICENSE.txt` and
`DOTNET-THIRD-PARTY-NOTICES.txt`.

- Source: <https://github.com/dotnet/runtime>
- License: <https://github.com/dotnet/runtime/blob/main/LICENSE.TXT>

## Tools downloaded at installation time

The public flasher does not embed these executables. It downloads the pinned
official release archives on the user's PC and verifies their SHA-256 hashes.

- esptool 5.3.1: <https://github.com/espressif/esptool>, GPL-2.0-or-later
- mklittlefs 4.0.2: <https://github.com/earlephilhower/mklittlefs>, MIT

## SpriteCollab material downloaded by the user

Sprite artwork is not distributed in this repository or the public flasher.
The local installer downloads it from
<https://github.com/PMDCollab/SpriteCollab>. SpriteCollab publishes its
contributor artwork under CC BY-NC 4.0 and records detailed attribution in
`tracker.json` and `credit_names.txt`; the flasher retains those files locally.
See the source project's license and attribution records before use.
