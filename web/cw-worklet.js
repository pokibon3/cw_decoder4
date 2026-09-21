//
//	CW Decoder — AudioWorklet プロセッサ
//
//	オーディオスレッドで動き、次の 3 つをやる:
//	  1. 入力を 8kHz へデシメート (ファームの「32kHz を 4 点平均」と同じ矩形窓)
//	  2. ADC カウント (0..4095、中心 2048) に直して WASM 中核へ渡す
//	  3. 表示用データをメインスレッドへ postMessage する
//
//	DSP をここで回すのは、タブが裏に回っても止まらないようにするため。
//	描画 (requestAnimationFrame) は裏になると止まるが、デコードは続く。
//
const TARGET_RATE = 8000;
const STATUS_WORDS = 18;
const POST_INTERVAL_MS = 30;        // 表示データの送出間隔 (約33fps)

class CWProcessor extends AudioWorkletProcessor {
	constructor() {
		super();
		this.core = null;
		this.ratio = sampleRate / TARGET_RATE;
		this.acc = 0;
		this.cnt = 0;
		this.phase = 0;
		this.gain = 1;
		this.scopeCursor = 0;
		this.lastPost = 0;
		this.out = new Float32Array(4096);
		this.outN = 0;
		// 取りこぼし検出: 実際に中核へ渡した 8kHz サンプル数と、
		// オーディオ時計から期待される数を突き合わせる。
		// オーディオスレッドが締め切りに間に合わないと入力が落ちるので、
		// デコード率が落ちたときにまずここを見る
		this.pushed = 0;
		this.t0 = 0;
		this.port.onmessage = (e) => this.onMessage(e.data);
	}

	async onMessage(msg) {
		if (msg.type === 'wasm') {
			const { instance } = await WebAssembly.instantiate(msg.bytes, {
				env: { cos: Math.cos, sin: Math.sin },
			});
			const e = instance.exports;
			e.__wasm_call_ctors?.();
			e.cw_init();
			this.core = e;
			this.inPtr = e.cw_in_ptr();
			this.inCap = e.cw_in_cap();
			this.stride = e.cw_scope_stride();
			this.specBins = e.cw_spec_bins();
			this.port.postMessage({ type: 'ready', rate: sampleRate, target: TARGET_RATE });
		} else if (msg.type === 'tone') {
			this.core?.cw_set_tone(msg.idx);
		} else if (msg.type === 'toneHz') {
			this.core?.cw_set_tone_hz(msg.hz);
		} else if (msg.type === 'cycleTone') {
			this.core?.cw_cycle_tone();
		} else if (msg.type === 'toggleMode') {
			this.core?.cw_toggle_mode();
		} else if (msg.type === 'reset') {
			this.core?.cw_reset();
			this.scopeCursor = 0;
			this.acc = this.cnt = this.phase = 0;
			this.pushed = 0;
			this.t0 = currentTime;
		} else if (msg.type === 'gain') {
			this.gain = msg.value;
		} else if (msg.type === 'drain') {
			this.drain();
		}
	}

	//	入力 128 サンプルを 8kHz へ落として WASM に積む
	decimate(ch) {
		for (let i = 0; i < ch.length; i++) {
			this.acc += ch[i];
			this.cnt++;
			this.phase += 1;
			if (this.phase >= this.ratio) {
				this.phase -= this.ratio;
				const y = (this.acc / this.cnt) * this.gain;
				this.acc = 0;
				this.cnt = 0;
				if (this.outN < this.out.length) this.out[this.outN++] = y;
			}
		}
	}

	flush() {
		if (this.outN === 0) return;
		const n = Math.min(this.outN, this.inCap);
		const dst = new Uint16Array(this.core.memory.buffer, this.inPtr, n);
		for (let i = 0; i < n; i++) {
			// Float ±1.0 → ADC カウント。ファームの ADC (1.65V バイアス +
			// ±2048 カウント) と同じ数値レンジに合わせるので、しきい値や
			// ノイズ床の扱いが実機とまったく同じになる
			let v = Math.round(2048 + this.out[i] * 2048);
			dst[i] = v < 0 ? 0 : v > 4095 ? 4095 : v;
		}
		this.core.cw_push(n);
		this.core.cw_run();
		this.pushed += n;
		this.outN = 0;
	}

	//	音源が止まると入力が途切れ、最後の文字が確定しないまま保留される
	//	(デコーダは「符号のあとに十分な無音が続いた」ことで文字を確定するため)。
	//	再生終了・停止のときに無音を流し込んで語間を作り、吐き出させる。
	//	実効入力レートの計算には入れない (止まったあとの分なので)
	drain() {
		if (!this.core) return;
		const total = TARGET_RATE * 2;          // 2 秒ぶん = 語間として十分
		for (let done = 0; done < total; done += this.inCap) {
			const n = Math.min(this.inCap, total - done);
			new Uint16Array(this.core.memory.buffer, this.inPtr, n).fill(2048);
			this.core.cw_push(n);
			this.core.cw_run();
		}
		this.post();
	}

	post() {
		const e = this.core;
		const maxCols = 512;
		e.cw_poll(this.scopeCursor, maxCols);
		const buf = e.memory.buffer;
		const st = new Uint32Array(buf, e.cw_status_ptr(), STATUS_WORDS);
		const nCols = st[3];
		const nChars = st[4];

		// 取りこぼしがあったら JS 側のカーソルを実際の先頭へ寄せ直す
		this.scopeCursor = st[1] + nCols;

		const cols = nCols
			? new Uint8Array(buf, e.cw_scope_ptr(), nCols * this.stride).slice()
			: new Uint8Array(0);
		const chars = nChars
			? new Uint8Array(buf, e.cw_chars_ptr(), nChars * 8).slice()
			: new Uint8Array(0);
		const spec = new Uint16Array(buf, e.cw_spec_ptr(), this.specBins).slice();

		this.port.postMessage({
			type: 'frame',
			status: new Uint32Array(st),      // WASM メモリからのコピー
			spec, cols, chars,
			colFirst: st[1],
			pushed: this.pushed,      // 累積サンプル数 (メイン側で区間レートにする)
			t: currentTime,
		}, [cols.buffer, chars.buffer, spec.buffer]);
	}

	process(inputs) {
		if (!this.core) return true;
		const ch = inputs[0]?.[0];
		if (ch) this.decimate(ch);
		this.flush();

		const now = currentTime * 1000;
		if (now - this.lastPost >= POST_INTERVAL_MS) {
			this.lastPost = now;
			this.post();
		}
		return true;
	}
}

registerProcessor('cw-processor', CWProcessor);
