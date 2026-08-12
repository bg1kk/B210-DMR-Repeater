<!--
Compiled and written by BG1KK.
Privatization and closed-source use are strictly forbidden.
GNU Radio components are copyrighted by their respective developers.
All other code copyright © BG1KK.
This copyright statement must be retained.
-->
# gr-dmr for GNU Radio 3.10

This directory packages the existing B210 DMR frame decoder as an
installable GNU Radio out-of-tree module. It installs:

- `libgnuradio-dmr.so`
- `gnuradio/dmr/frame_decoder.h`
- the `gnuradio-dmr` CMake package and `gnuradio::gnuradio-dmr` target
- the `gnuradio.dmr` Python module

The DMR BPTC, Hamming, and Golay implementations are compiled from the
OP25 `gr-op25_repeater/lib` source tree. Set `OP25_REPEATER_SOURCE_DIR`
when OP25 is not located at the default Pi deployment path.

```bash
cmake -S ~/dmr-b210/gr-dmr -B ~/dmr-b210/gr-dmr/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr/local \
  -DOP25_REPEATER_SOURCE_DIR=~/source/op25/op25/gr-op25_repeater/lib \
  -DENABLE_PYTHON=ON
cmake --build ~/dmr-b210/gr-dmr/build --parallel
sudo cmake --install ~/dmr-b210/gr-dmr/build
sudo ldconfig
```

Verify both interfaces after installation:

```bash
python3 -c 'from gnuradio import dmr; print(dmr.frame_decoder(4800, 0, -1, False))'
cmake -S ~/dmr-b210 -B ~/dmr-b210/build -G Ninja
```

The module and the OP25-derived error-correction code are distributed
under GPL-compatible terms; retain the source copyright and license
notices when redistributing binaries.
