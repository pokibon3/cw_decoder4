#
#	CW Decoder 4 の PC シミュレータ (Mac / Linux)
#
#	必要なもの: SDL2 (Mac は brew install sdl2)、音声ファイルを使うなら ffmpeg。
#	LovyanGFX は PlatformIO が取ってきたもの (.pio/libdeps/) を使うので、
#	先に一度ファームウェアをビルドしておく (pio run -e esp32dev)。
#
#	make            ビルド (build/cwsim)
#	make run        合成した CW で起動
#	make shots      取扱説明書 (docs/manual.md) の画面写真を撮り直す (../docs/images/)
#
#	画面写真は build/cwsim-hd (Mac 専用) で撮る。ファームの描画を 2 倍の座標で描き、
#	文字の字形だけ元の書体のフォントでなめらかに描き直したもの (sim_lgfx.h / sim_hdfont.cpp)。
#	フォントは初回に fetch_fonts.sh が build/fonts/ へ取ってくる (要ネット接続)。
#	ふだんの動作確認は実機と同じ画素の build/cwsim を使う
#
LGFX_DIR ?= ../.pio/libdeps/esp32dev/LovyanGFX
LGFX_SRC = $(LGFX_DIR)/src/lgfx

CXX ?= c++
CC ?= cc
SDL_CFLAGS := $(shell sdl2-config --cflags)
SDL_LIBS := $(shell sdl2-config --libs)

COMMON = -O2 -g -DCWDEC_SIM -Wno-deprecated-declarations \
	-Ishim -I../src -I../lib/float_fft -I$(LGFX_DIR)/src $(SDL_CFLAGS)
CXXFLAGS = -std=c++17 $(COMMON)
CFLAGS = $(COMMON)

# ファームウェアのソース (NTP / 自動更新 / ADC は sim_main.cpp が代わりを持つ。
# ota.cpp は WiFi などのスタブ (shim/) の上で画面だけ動く)
FW_SRCS = main.cpp display.cpp setup.cpp clock.cpp timeset.cpp touchcal.cpp \
	scopelog.cpp dsp.cpp decoder.cpp decode.cpp ota.cpp

LGFX_SRCS = $(wildcard $(LGFX_SRC)/v1/*.cpp) $(wildcard $(LGFX_SRC)/v1/misc/*.cpp) \
	$(LGFX_SRC)/v1/panel/Panel_Device.cpp $(LGFX_SRC)/v1/panel/Panel_FrameBufferBase.cpp \
	$(wildcard $(LGFX_SRC)/v1/platforms/sdl/*.cpp)
LGFX_CSRCS = $(wildcard $(LGFX_SRC)/utility/*.c) $(wildcard $(LGFX_SRC)/Fonts/efont/*.c) \
	$(wildcard $(LGFX_SRC)/Fonts/IPA/*.c)

OBJS = $(addprefix build/fw/,$(FW_SRCS:.cpp=.o)) build/fw/float_fft.o build/sim_main.o \
	$(patsubst $(LGFX_SRC)/%.cpp,build/lgfx/%.o,$(LGFX_SRCS)) \
	$(patsubst $(LGFX_SRC)/%.c,build/lgfx/%.o,$(LGFX_CSRCS))

HDRS = $(wildcard ../src/*.h) $(wildcard shim/*.h) sim_lgfx.h

LGFX_OBJS = $(patsubst $(LGFX_SRC)/%.cpp,build/lgfx/%.o,$(LGFX_SRCS)) \
	$(patsubst $(LGFX_SRC)/%.c,build/lgfx/%.o,$(LGFX_CSRCS))
HD_OBJS = $(addprefix build/hd/,$(FW_SRCS:.cpp=.o)) build/fw/float_fft.o build/hd/sim_main.o \
	build/hd/sim_hdfont.o $(LGFX_OBJS)

build/cwsim: $(OBJS)
	$(CXX) -o $@ $(OBJS) $(SDL_LIBS) -lpthread

build/cwsim-hd: $(HD_OBJS)
	$(CXX) -o $@ $(HD_OBJS) $(SDL_LIBS) -lpthread \
		-framework CoreText -framework CoreGraphics -framework CoreFoundation

build/hd/%.o: ../src/%.cpp $(HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DCWDEC_SIM_HD -c $< -o $@

build/hd/%.o: %.cpp $(HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -DCWDEC_SIM_HD -c $< -o $@

build/fw/%.o: ../src/%.cpp $(HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/fw/float_fft.o: ../lib/float_fft/float_fft.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/sim_main.o: sim_main.cpp $(HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

build/lgfx/%.o: $(LGFX_SRC)/%.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -w -c $< -o $@

build/lgfx/%.o: $(LGFX_SRC)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -w -c $< -o $@

$(LGFX_SRC)/v1/LGFXBase.cpp:
	@echo "LovyanGFX が見つかりません ($(LGFX_DIR))。先に pio run -e esp32dev を実行してください" >&2
	@false

run: build/cwsim
	./build/cwsim

# 取扱説明書 (docs/manual.md) に貼る画面。設定は毎回まっさらな状態から撮る
SHOT_TEXT = CQ CQ DE JA1QRP JA1QRP K
SHOT_TEXT_JP = コンニチハ ゴキゲン イカガデスカ キョウハ ヨイ テンキデス
build/fonts/ipag.ttf:
	sh fetch_fonts.sh

shots: build/cwsim-hd build/fonts/ipag.ttf
	rm -f build/shots-nvs.txt
	./build/cwsim-hd --nvs build/shots-nvs.txt --shots ../docs/images \
		--text "$(SHOT_TEXT)" --script @shots.txt
	rm -f build/shots-nvs.txt
	./build/cwsim-hd --nvs build/shots-nvs.txt --shots ../docs/images \
		--text "$(SHOT_TEXT_JP)" --script @shots-jp.txt

clean:
	rm -f build/cwsim build/cwsim-hd $(OBJS) $(HD_OBJS)

.PHONY: run shots clean
