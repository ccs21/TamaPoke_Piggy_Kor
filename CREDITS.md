# Credits

## Project lineage

- Original project: [socquique/TamaPoke](https://github.com/socquique/TamaPoke),
  created by Quique Tortosa and published under the MIT License.
- Initial development base: the
  [tamapoke-expanded-update](https://github.com/ShadowEnemyx/TamaPoke/tree/tamapoke-expanded-update)
  branch maintained by ShadowEnemyx.
- Korean edition, current firmware features, UI, power management,
  communication, minigames and Windows flasher: ccs21.

The original copyright and MIT notice are retained in `LICENSE`.

## Sprite source used by the local installer

The repository and public flasher do not bundle Pokémon sprite images or
packed sprite files. During installation, the user's PC downloads the required
source files directly from
[PMDCollab/SpriteCollab](https://github.com/PMDCollab/SpriteCollab), selects the
animations used by the firmware and creates local TPK3 files.

SpriteCollab artwork is contributed by its community and is published by that
project under [CC BY-NC 4.0](https://github.com/PMDCollab/SpriteCollab/blob/master/LICENSE.md).
Per-species and per-author attribution is maintained by SpriteCollab in
`tracker.json` and `credit_names.txt`. The flasher downloads and retains those
files with its local cache. The license only covers rights its licensors are
authorized to grant; no endorsement or additional trademark rights are
implied.

## Data, font and libraries

- Species and move reference data: [PokéAPI](https://pokeapi.co)
- Korean bitmap font source: Noto Sans KR, SIL Open Font License 1.1
- Arduino GFX Library: moononournation and upstream contributors
- SensorLib and XPowersLib: Lewis He / lewisxhe
- Arduino ESP32 Core and ESP-IDF components: Espressif Systems
- NAudio: Mark Heath and contributors
- Board and pinout: Waveshare ESP32-S3-Touch-AMOLED-1.75 / 1.75C

See `THIRD_PARTY_NOTICES.md` for license details.

## User-supplied assets

Loading, capture, Snorlax and Diglett images and all additional scene/effect
audio are not included. Each user supplies those files locally in
`Additional_assets.zip`. The flasher processes them only on that user's PC and
does not upload them. Users must provide only files they created themselves or
for which they hold the rights needed for software embedding, editing and use.

## Unofficial project notice

This is an unofficial, non-commercial fan project and is not affiliated with
or endorsed by The Pokémon Company, Nintendo, Creatures, GAME FREAK or Bandai.
Related names, characters, designs and trademarks belong to their respective
rights holders.
