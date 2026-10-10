720 Degrees - Game folder
=========================

Drop the arcade ROM files into THIS folder (not a sub-folder, not zipped).
The ROMs are not included in this repository; you must supply your own copy
of the "720 Degrees (rev 4)" set (MAME short name: 720).

Only the ROM files go here. The build puts 720degrees.exe, editor720.exe and
SDL3.dll in the folder above this one, and both programs look for their ROMs
in this Game folder.

Required files (37):

  File               Size     CRC32      Contents
  -----------------  -------  ---------  --------------------------------
  136047-3126.7lm    16384    43abd367   main program (fixed, even bytes)
  136047-3127.7mn    16384    772e1e5b   main program (fixed, odd bytes)
  136047-3128.6fh    65536    bf6f425b   main program banks (even)
  136047-4131.6mn    65536    2ea8a20f   main program banks (odd)
  136047-1129.6hj    65536    eabf0b01   main program banks (even)
  136047-1132.6p     65536    a24f333e   main program banks (odd)
  136047-1130.6k     65536    93fba845   main program banks (even)
  136047-1133.6r     65536    53c177be   main program banks (odd)
  136047-2134.2a     16384    0db4ca28   sound program
  136047-1135.2b     16384    b1f157d0   sound program
  136047-2136.2cd    16384    00b06bec   sound program
  136047-1121.6a     32768    7adb5f9a   playfield tiles
  136047-1122.6b     32768    41b60141   playfield tiles
  136047-1123.7a     32768    501881d5   playfield tiles
  136047-1124.7b     32768    096f2574   playfield tiles
  136047-1117.6c     32768    5a55f149   playfield tiles
  136047-1118.6d     32768    9bb2429e   playfield tiles
  136047-1119.7d     32768    8f7b20e5   playfield tiles
  136047-1120.7c     32768    46af6d35   playfield tiles
  136047-1109.6t     65536    0a46b693   motion objects (sprites)
  136047-1110.6sr    65536    457d7e38   motion objects
  136047-1111.6p     65536    ffad0a5b   motion objects
  136047-1112.6n     65536    06664580   motion objects
  136047-1113.6m     65536    7445dc0f   motion objects
  136047-1114.6l     65536    23eaceb0   motion objects
  136047-1115.6kj    65536    0cc8de53   motion objects
  136047-1116.6jh    65536    2d8f1369   motion objects
  136047-1101.5t     65536    2ac77b80   motion objects
  136047-1102.5sr    65536    f19c3b06   motion objects
  136047-1103.5p     65536    78f9ab90   motion objects
  136047-1104.5n     65536    77ce4a7f   motion objects
  136047-1105.5m     65536    bef5a025   motion objects
  136047-1106.5l     65536    92a159c8   motion objects
  136047-1107.5kj    65536    0a94a3ef   motion objects
  136047-1108.5jh    65536    9815eda6   motion objects
  136047-1125.4t     16384    6b7e2328   alphanumerics (text font)
  720-eeprom.bin       512    5d2acf11   default EEPROM (settings / high scores)

If you have a MAME "720.zip", extract its contents here.
