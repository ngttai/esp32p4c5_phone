# Emote pack

Eye animations for the AI Assistant app, in EAF format (360 x 360, 4-bit, Huffman): the 21 files of the
`eaf_360x360` folder of Espressif's [Emote Pack.zip](https://dl.espressif.com/AE/Emote%20Pack.zip), linked from
[espressif2022/esp_emote_gen_player](https://github.com/espressif2022/esp_emote_gen_player), that the 22 emotion
names of the Xiaozhi protocol use (`assistant_emote.cpp` has the table). The other 10 of the pack (badminton,
music, mute, ...) are not used. The downloaded pack has no license file: the license of these assets is not
confirmed.

A memory card can replace any of them: `/sdcard/appdata/assistant/emote/<emotion>.eaf` (for example `happy.eaf`), or
`pack_<name>.eaf` as named in the pack, so a folder of the pack (`eaf_466x466`, ...) can be copied as it is.
Up to 480 x 480; other sizes are scaled into the 360 x 360 face.
