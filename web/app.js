//
//	CW Decoder (Web) — メインスレッド
//
//	担当は 3 つ:
//	  - 音声入力元の切り替え (マイク/ライン・タブ音声・ファイル・内蔵テスト信号)
//	  - AudioWorklet から届く表示データの受け取り
//	  - 描画 (デコード文字 / FFT / オシロスコープ)
//
//	DSP とデコーダは AudioWorklet 内の WebAssembly (cwcore.wasm) が持つ。
//	ここには信号処理のロジックは一切入れない (実機と同じ中核を使うため)。
//
const SAMPLE_RATE_TARGET = 8000;
const SPEC_BINS = 65;                     // bin 0..64 (31.25Hz/bin)
const HZ_PER_BIN = SAMPLE_RATE_TARGET / 256;

//	status[] のインデックス (api.cpp の cw_status_t と対応)
const ST = {
	scopeTotal: 0, scopeFirst: 1, scopeLost: 2, scopeN: 3, charN: 4,
	wpm: 5, toneHz: 6, toneIdx: 7, toneAuto: 8, peakHz: 9, gateBw: 10,
	periodQ8: 11, colMsX10: 12, inputPeak: 13, inputPct: 14,
	mode: 15, gate: 16, clip: 17,
};

//	実機のパレット (display.cpp の lgfx::color565(r,g,b) と同じ値)
const C = {
	panelBg: '#020810', frame: '#283c55', label: '#647d9b',
	eqFill: '#08412f', eqLine: '#3ce6a0', eqPeak: '#d2d7e1',
	marker: '#324b82', band: '#0a182a', toneBand: '#463410',
	raw: '#0082af', env: '#fac775', gate: '#5adc78',
	lvlLo: '#2dc878', lvlMid: '#e6b43c', lvlHi: '#eb463c',
	textNew: '#adff2f',
};

//==================================================================
//	JIS X 0201 カナ → 全角カタカナ (display.cpp と同じ表)
//==================================================================
const KANA_CP = [
	0x3002, 0x300C, 0x300D, 0x3001, 0x30FB, 0x30F2,
	0x30A1, 0x30A3, 0x30A5, 0x30A7, 0x30A9,
	0x30E3, 0x30E5, 0x30E7, 0x30C3, 0x30FC,
	0x30A2, 0x30A4, 0x30A6, 0x30A8, 0x30AA,
	0x30AB, 0x30AD, 0x30AF, 0x30B1, 0x30B3,
	0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD,
	0x30BF, 0x30C1, 0x30C4, 0x30C6, 0x30C8,
	0x30CA, 0x30CB, 0x30CC, 0x30CD, 0x30CE,
	0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB,
	0x30DE, 0x30DF, 0x30E0, 0x30E1, 0x30E2,
	0x30E4, 0x30E6, 0x30E8,
	0x30E9, 0x30EA, 0x30EB, 0x30EC, 0x30ED,
	0x30EF, 0x30F3, 0x309B, 0x309C,
];
const DAKUTEN_OK = new Set([
	0x30A6, 0x30AB, 0x30AD, 0x30AF, 0x30B1, 0x30B3,
	0x30B5, 0x30B7, 0x30B9, 0x30BB, 0x30BD,
	0x30BF, 0x30C1, 0x30C4, 0x30C6, 0x30C8,
	0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB,
]);
const HANDAKUTEN_OK = new Set([0x30CF, 0x30D2, 0x30D5, 0x30D8, 0x30DB]);

function codepointOf(ch) {
	if (ch === 5) return 0x300C;              // ホレ (和文開始) → 「
	if (ch === 6) return 0x300D;              // ラタ (和文終了) → 」
	if (ch >= 0xA1 && ch <= 0xDF) return KANA_CP[ch - 0xA1];
	if (ch < 0x80) return ch;
	return 0x2A;                              // '*'
}

//==================================================================
//	DOM
//==================================================================
const $ = (id) => document.getElementById(id);
const el = {
	state: $('state'), wpm: $('wpm'), toneHz: $('toneHz'), peakHz: $('peakHz'),
	lvlBar: $('lvlBar'), lvlTx: $('lvlTx'), clip: $('clip'),
	btnMode: $('btnMode'), btnTone: $('btnTone'),
	text: $('text'), fft: $('fft'), scope: $('scope'),
	gain: $('gain'), gainV: $('gainV'), inRate: $('inRate'),
};
const tHead = document.createElement('span');
const tNew = document.createElement('span');
tNew.className = 'new';
el.text.append(tHead, tNew);

//==================================================================
//	オーディオエンジン
//==================================================================
let ctx = null;         // AudioContext
let node = null;        // AudioWorkletNode (cw-processor)
let sink = null;        // 無音の終端 (ワークレットを回し続けるため)
let monitor = null;     // モニター音の GainNode
let srcNode = null;     // 現在の入力ノード
let chain = [];         // 入力ノードと worklet の間に挟んだノード (停止時に外す)
let srcStream = null;   // getUserMedia / getDisplayMedia のストリーム
let ready = false;

function setState(msg, isErr = false) {
	el.state.textContent = msg;
	el.state.className = isErr ? 'err' : '';
}

//	AudioWorklet モジュールと WASM はブラウザにキャッシュされる。ローカルで
//	編集しながら試すときに古いものを掴み続けるので、localhost のときだけ
//	キャッシュを外す (公開時は普通にキャッシュさせる)
const DEV = location.hostname === 'localhost' || location.hostname === '127.0.0.1';
const bust = (url) => (DEV ? `${url}?t=${Date.now()}` : url);

//	単一ファイル版 (build.sh が作る cw-decoder.html) では、ワークレットと
//	wasm が __CW_BUNDLE に埋め込まれている。file:// で開いたときは外部
//	ファイルを fetch できない (不透明オリジンとして拒否される) ので、
//	埋め込みがあればそちらを使う
const BUNDLE = globalThis.__CW_BUNDLE;

async function addWorkletModule() {
	if (BUNDLE?.worklet) {
		// Blob URL はページと同一オリジン扱いなので file:// でも addModule できる
		const url = URL.createObjectURL(new Blob([BUNDLE.worklet], { type: 'text/javascript' }));
		try { await ctx.audioWorklet.addModule(url); }
		finally { URL.revokeObjectURL(url); }
	} else {
		await ctx.audioWorklet.addModule(bust('cw-worklet.js'));
	}
}

async function loadWasmBytes() {
	if (BUNDLE?.wasm) {
		const bin = atob(BUNDLE.wasm);
		const u8 = new Uint8Array(bin.length);
		for (let i = 0; i < bin.length; i++) u8[i] = bin.charCodeAt(i);
		return u8.buffer;
	}
	return (await fetch(bust('cwcore.wasm'))).arrayBuffer();
}

async function ensureEngine() {
	if (ctx) {
		if (ctx.state === 'suspended') await ctx.resume();
		return;
	}
	// 48kHz を要求して自前で 1/6 デシメートする。8000 を直接要求すると
	// ブラウザ内部のリサンプラが挟まり、フィルタ特性が環境依存になる
	ctx = new AudioContext({ sampleRate: 48000, latencyHint: 'interactive' });

	// AudioWorklet はサーバー経由でないと読み込めない。
	//   file:// : ページのオリジンが null (不透明) になるため、ワークレットの
	//             モジュールが何であれ cross-origin 扱いで拒否される。
	//             埋め込みを blob: や data: にしても blob:null/... となり同じ。
	//   data:   : そもそも安全なコンテキストでないので audioWorklet が生えない
	//   http:// : localhost 以外は安全なコンテキストでないので同上
	// いずれも素のエラーでは原因が分からないので、ここで説明を出す
	if (location.protocol === 'file:') {
		throw new Error(
			'file:// で直接開くと AudioWorklet を読み込めません ' +
			'(オリジンが null になり cross-origin として拒否されます)。' +
			'web/serve.sh を実行して http://localhost:8777/ から開いてください。');
	}
	if (!ctx.audioWorklet) {
		throw new Error(
			'この開き方では AudioWorklet が使えません (安全なコンテキストではありません)。' +
			'https か localhost で配信してください。web/serve.sh が使えます。');
	}
	await addWorkletModule();
	// 明示的にモノラルにする。こうしないと process() の inputs[0][0] が
	// 「L チャンネルだけ」になり、ステレオ音源の R が黙って捨てられる
	node = new AudioWorkletNode(ctx, 'cw-processor', {
		numberOfInputs: 1, numberOfOutputs: 1,
		channelCount: 1, channelCountMode: 'explicit', channelInterpretation: 'speakers',
	});
	node.port.onmessage = onWorkletMessage;

	// ワークレットは出力を出さないが、終端に繋がないと process() が
	// 呼ばれ続けない環境があるので、ゲイン 0 で destination に繋ぐ
	sink = ctx.createGain();
	sink.gain.value = 0;
	node.connect(sink).connect(ctx.destination);

	monitor = ctx.createGain();
	monitor.gain.value = 0;
	monitor.connect(ctx.destination);

	const bytes = await loadWasmBytes();
	node.port.postMessage({ type: 'wasm', bytes }, [bytes]);
	await new Promise((res, rej) => {
		const t0 = performance.now();
		const t = setInterval(() => {
			if (ready) { clearInterval(t); res(); }
			else if (performance.now() - t0 > 10000) {
				clearInterval(t);
				rej(new Error('cwcore.wasm の読み込みに失敗しました'));
			}
		}, 10);
	});
}

function stopSource() {
	if (srcNode) { try { srcNode.disconnect(); } catch {} }
	if (srcNode && srcNode.stop) { try { srcNode.stop(); } catch {} }
	if (srcStream) srcStream.getTracks().forEach((t) => t.stop());
	for (const c of chain) { try { c.disconnect(); } catch {} }
	chain = [];
	srcNode = null;
	srcStream = null;
	if (monitor && ctx) monitor.gain.setTargetAtTime(0, ctx.currentTime, 0.01);
	// ファイルのトランスポートは filePlaying が持つので、ここでは触らない
	$('testStop').disabled = true;
	$('testPlay').disabled = false;
}

//	入力ノードを差し替える。monitorOn で生音をスピーカーへ出すかを決める。
//	stream は getUserMedia / getDisplayMedia のもの (停止時に track を止めるため)
function attachSource(n, monitorOn, stream = null) {
	stopSource();                       // 先に古い入力を完全に片付ける
	srcNode = n;
	srcStream = stream;
	monitorOn_ = monitorOn;
	wireChain();
	node.port.postMessage({ type: 'reset' });
}

//	ステレオ入力のどの系統をデコードに回すか。
//	無線機をステレオ I/F の片チャンネルだけに繋いでいる場合、L+R の
//	ダウンミックスは空いている側のノイズを足し込むので S/N が約 3dB 落ちる。
//	入力元を問わず効くように、入力ノードと worklet の間で切り替える
let monitorOn_ = false;
let bpf = null;         // 入力段バンドパス (実機のアナログ BPF 相当)

function wireChain() {
	if (!srcNode) return;
	for (const c of chain) { try { c.disconnect(); } catch {} }
	chain = [];
	try { srcNode.disconnect(); } catch {}

	const mode = $('chSel').value;
	let out = srcNode;
	if (mode !== 'MIX') {
		const sp = ctx.createChannelSplitter(2);
		const g = ctx.createGain();
		srcNode.connect(sp);
		sp.connect(g, mode === 'R' ? 1 : 0, 0);
		chain.push(sp, g);
		out = g;
	}

	// 入力段バンドパス。実機は ADC の手前に多重帰還型の BPF
	// (中心 700Hz / Q=5) が入っており、ファームのしきい値やノイズ床の
	// 扱いはその帯域制限された入力を前提に実機調整されている。
	// Web 版は素の全帯域音声が入るので、これが無いと帯域外ノイズが
	// サイドビンとノイズ床を押し上げてデコード率が大きく落ちる。
	// (実測: この BPF 無しで 44% → 実機相当の設定で 93%)
	// 実機と同じくデシメートの前 (= ADC の手前) に置く。
	// 副次的にエイリアシングで折り返す帯域外成分も減る
	bpf = null;
	if ($('bpfOn').checked) {
		bpf = ctx.createBiquadFilter();
		bpf.type = 'bandpass';          // RBJ cookbook 2次 BPF = MFB BPF と同じ伝達関数
		bpf.frequency.value = bpfCenter();
		bpf.Q.value = +$('bpfQ').value;
		out.connect(bpf);
		chain.push(bpf);
		out = bpf;
	}

	out.connect(node);
	// モニターは常につないでおき、聞こえるかどうかは音量だけで決める。
	// 接続/切断で切り替えると再生中に反映されない (chain を張り直すまで
	// 効かない) ので、ここでは必ず繋ぐ
	if (monitor) {
		out.connect(monitor);
		applyMonitor();
	}
}

//	モニター音の ON/OFF。3 つのタブのチェックは同じ 1 本の経路を指すので
//	状態を揃える。切り替えでプツッと鳴らないよう短いランプをかける
function applyMonitor(on) {
	if (on !== undefined) monitorOn_ = !!on;
	for (const id of ['micMonitor', 'tabMonitor', 'fileMonitor', 'testMonitor']) {
		const el = $(id);
		if (el) el.checked = monitorOn_;
	}
	if (!monitor || !ctx) return;
	monitor.gain.setTargetAtTime(monitorOn_ ? 1 : 0, ctx.currentTime, 0.01);
}

//	BPF の中心周波数。既定はトーン追従で、AUTO の同調先へ合わせる。
//	実機のハードは 700Hz 固定だが、Web 版にその制約は無く、実測でも
//	トーンが 700Hz から離れた音源ほど有利だった (下の README 参照)。
//	実機と同じ条件で比べたいときは「700Hz 固定」を選ぶ
function bpfCenter() {
	if ($('bpfCenter').value === 'track') {
		const hz = status[ST.toneHz];
		if (hz >= 400 && hz <= 1200) return hz;
	}
	return 700;
}

//	トーン追従時に中心を追わせる (再生を止めずに滑らかに動かす)
function updateBpf() {
	const label = $('bpfNow');
	if (!bpf || !ctx) { if (label) label.textContent = ''; return; }
	const f = bpfCenter();
	if (Math.abs(bpf.frequency.value - f) >= 1) {
		bpf.frequency.setTargetAtTime(f, ctx.currentTime, 0.05);
	}
	const q = +$('bpfQ').value;
	if (bpf.Q.value !== q) bpf.Q.value = q;
	// 実際に効いている中心を出す。追従しているかが画面で分かるように
	if (label) label.textContent = `${Math.round(bpf.frequency.value)}Hz`;
}

//	入力ゲイン (dB)。デコーダへ渡す信号にだけ効き、モニター音には効かない。
//	デコーダのしきい値はノイズ床基準の相対値なので本来レベルには鈍感だが、
//	ADC 相当のレンジ (±2048 カウント) で潰れるとサイドビンが上がって
//	デコード率が落ちるため、クリップさせない範囲に収めるのが目的
function setGain(db) {
	db = Math.max(-40, Math.min(40, Math.round(db)));
	el.gain.value = db;
	el.gainV.textContent = `${db > 0 ? '+' : ''}${db} dB`;
	node?.port.postMessage({ type: 'gain', value: Math.pow(10, db / 20) });
	return db;
}

//==================================================================
//	入力元 1: マイク / ライン入力
//==================================================================
async function listMics() {
	const devs = await navigator.mediaDevices.enumerateDevices();
	const ins = devs.filter((d) => d.kind === 'audioinput');
	const sel = $('micDev');
	const prev = sel.value;
	sel.innerHTML = '';
	for (const d of ins) {
		const o = document.createElement('option');
		o.value = d.deviceId || '';
		o.textContent = d.label || `入力 ${sel.length + 1}`;
		sel.append(o);
	}
	if (prev) sel.value = prev;
	if (!ins.length) {
		sel.innerHTML = '<option value="">既定の入力 (デバイス一覧はまだ取れていません)</option>';
	}
}

async function startMic() {
	await ensureEngine();
	// value が空のときは既定の入力を使う (exact に空や飾りの文字列を
	// 渡すと OverconstrainedError になる)
	const id = $('micDev').value || '';
	// エコーキャンセル / ノイズ抑制 / AGC は必ず切る。有効だとブラウザが
	// CW を「ノイズ」として消してしまい、まったくデコードできなくなる
	// channelCount は指定しない。1 を強制するとブラウザが L+R をダウンミックス
	// するので、無線機を片チャンネルだけに繋いでいると空き側のノイズを拾う。
	// どの系統を使うかは pickChannel() で選ぶ
	const constraints = {
		audio: {
			deviceId: id ? { exact: id } : undefined,
			echoCancellation: false, noiseSuppression: false,
			autoGainControl: false,
		},
	};
	const stream = await navigator.mediaDevices.getUserMedia(constraints);
	await listMics();                       // 許可後はデバイス名が取れる
	const src = ctx.createMediaStreamSource(stream);
	attachSource(src, $('micMonitor').checked, stream);

	// ブラウザが前処理の無効化を実際に受け入れたかを確認する。
	// 受け入れていないと CW が「ノイズ」として消され、デコード率が大きく落ちる
	const track = stream.getAudioTracks()[0];
	const st = track.getSettings();
	const on = ['echoCancellation', 'noiseSuppression', 'autoGainControl'].filter((k) => st[k]);
	const ch = `${src.channelCount}ch → ${$('chSel').value}`;
	setState(`マイク/ライン: ${track.label || '(名称なし)'} — ` +
		`${st.sampleRate || ctx.sampleRate}Hz / ${ch}` +
		(on.length ? ` — ⚠ ${on.join(' / ')} が無効化できていません (デコード率が落ちます)` : ' / 前処理なし'),
		on.length > 0);
}

//==================================================================
//	入力元 2: タブ音声 (getDisplayMedia)
//==================================================================
async function startTab() {
	await ensureEngine();
	if (!navigator.mediaDevices.getDisplayMedia) {
		setState('このブラウザはタブ音声の取り込みに対応していません (Chrome 系で試してください)', true);
		return;
	}
	const stream = await navigator.mediaDevices.getDisplayMedia({
		video: true,
		audio: { echoCancellation: false, noiseSuppression: false, autoGainControl: false },
	});
	stream.getVideoTracks().forEach((t) => t.stop());   // 映像は使わない
	if (!stream.getAudioTracks().length) {
		stream.getTracks().forEach((t) => t.stop());
		setState('音声が共有されていません。共有ダイアログで「タブの音声も共有する」を ON にしてください', true);
		return;
	}
	attachSource(ctx.createMediaStreamSource(stream), $('tabMonitor').checked, stream);
	setState(`タブ音声: ${stream.getAudioTracks()[0].label || '(名称なし)'}`);
}

//==================================================================
//	入力元 3: 音声ファイル
//==================================================================
let fileBuffer = null;
//	AudioBufferSourceNode は一時停止もシークもできない (start は 1 回だけ) ので、
//	再生位置を自前で持ち、操作のたびにノードを作り直して offset 付きで start する。
//	filePos は「今の source が start された時点の位置」で、再生中の現在位置は
//	そこに経過時間を足して求める
let filePos = 0;            // 秒
let filePlaying = false;
let fileT0 = 0;             // start した時点の ctx.currentTime
let fileSeeking = false;    // シークバーをドラッグ中は表示を追従させない

const SEEK_STEP = 5;        // 巻き戻し / 早送りの秒数

function fileNow() {
	if (!fileBuffer) return 0;
	const t = filePlaying ? filePos + (ctx.currentTime - fileT0) : filePos;
	return Math.max(0, Math.min(fileBuffer.duration, t));
}

const mmss = (t) => `${Math.floor(t / 60)}:${String(Math.floor(t % 60)).padStart(2, '0')}`;

function fileUpdateUI() {
	const has = !!fileBuffer;
	for (const id of ['fileHome', 'fileBack', 'filePlay', 'fileFwd', 'fileEnd', 'fileSeek']) {
		$(id).disabled = !has;
	}
	$('filePlay').innerHTML = filePlaying ? '&#10074;&#10074;' : '&#9654;';
	$('filePlay').title = filePlaying ? '一時停止' : '再生';
	if (!has) {
		$('fileTime').textContent = '--:-- / --:--';
		return;
	}
	const t = fileNow(), d = fileBuffer.duration;
	$('fileTime').textContent = `${mmss(t)} / ${mmss(d)}`;
	if (!fileSeeking) $('fileSeek').value = Math.round((t / d) * 1000);
}

//	pos 秒から再生を始める。位置が飛ぶと符号の途中から入ることになるので、
//	attachSource が投げる reset でデコーダの状態も作り直される
function fileStartAt(pos) {
	filePos = Math.max(0, Math.min(fileBuffer.duration - 0.01, pos));
	const src = ctx.createBufferSource();
	src.buffer = fileBuffer;
	attachSource(src, $('fileMonitor').checked);
	src.onended = () => {
		// stop() で止めたときも呼ばれるので、末尾まで来た場合だけ後始末する
		if (!filePlaying) return;
		if (fileNow() >= fileBuffer.duration - 0.05) {
			filePlaying = false;
			filePos = fileBuffer.duration;
			drainDecoder();             // 末尾の文字を取りこぼさない
			fileUpdateUI();
		}
	};
	src.start(0, filePos);
	fileT0 = ctx.currentTime;
	filePlaying = true;
	fileUpdateUI();
}

async function filePlayPause() {
	await ensureEngine();
	if (!fileBuffer) return;
	if (filePlaying) {
		filePos = fileNow();
		filePlaying = false;
		drainDecoder();                 // 保留中の文字を吐き出してから止める
		stopSource();
		fileUpdateUI();
	} else {
		if (filePos >= fileBuffer.duration - 0.05) filePos = 0;   // 末尾なら頭から
		fileStartAt(filePos);
	}
}

//	再生中でも止まっていても同じように位置だけ動かす
async function fileSeek(pos) {
	await ensureEngine();
	if (!fileBuffer) return;
	const wasPlaying = filePlaying;
	if (wasPlaying) { filePlaying = false; stopSource(); }
	filePos = Math.max(0, Math.min(fileBuffer.duration, pos));
	if (wasPlaying && filePos < fileBuffer.duration - 0.05) fileStartAt(filePos);
	else fileUpdateUI();
}

async function loadFile(file) {
	await ensureEngine();
	const buf = await file.arrayBuffer();
	fileBuffer = await ctx.decodeAudioData(buf);
	filePos = 0;
	filePlaying = false;

	// 音源のピークを測って、-12 dBFS あたりに来るようゲインを自動設定する。
	// 通常の音声ファイルはフルスケール近くまで振れているので、素通しだと
	// ADC 相当のレンジで張り付いて (CLIP) デコード率が落ちる
	let peak = 0;
	for (let c = 0; c < fileBuffer.numberOfChannels; c++) {
		const d = fileBuffer.getChannelData(c);
		for (let i = 0; i < d.length; i++) {
			const a = Math.abs(d[i]);
			if (a > peak) peak = a;
		}
	}
	const db = peak > 0 ? setGain(-12 - 20 * Math.log10(peak)) : 0;
	const peakDb = peak > 0 ? (20 * Math.log10(peak)).toFixed(1) : '-inf';
	setState(`ファイル: ${file.name} — ${fileBuffer.duration.toFixed(1)}秒 / ` +
		`${fileBuffer.sampleRate}Hz / ピーク ${peakDb} dBFS → ゲイン ${db > 0 ? '+' : ''}${db} dB に自動調整`);
	fileUpdateUI();
	// 選んだらそのまま先頭から再生する (毎回 ▶ を押す手間を省く)
	fileStartAt(0);
}

//	保留されている最後の文字を吐き出させる (再生終了・停止時)
function drainDecoder() {
	node?.port.postMessage({ type: 'drain' });
}

function playBuffer(buffer, monitorOn, onended) {
	const s = ctx.createBufferSource();
	s.buffer = buffer;
	attachSource(s, monitorOn);
	s.onended = onended;
	s.start();
	return s;
}

//==================================================================
//	入力元 4: 内蔵テスト信号 (合成 CW)
//==================================================================
const MORSE = {
	A: '.-', B: '-...', C: '-.-.', D: '-..', E: '.', F: '..-.', G: '--.',
	H: '....', I: '..', J: '.---', K: '-.-', L: '.-..', M: '--', N: '-.',
	O: '---', P: '.--.', Q: '--.-', R: '.-.', S: '...', T: '-', U: '..-',
	V: '...-', W: '.--', X: '-..-', Y: '-.--', Z: '--..',
	0: '-----', 1: '.----', 2: '..---', 3: '...--', 4: '....-',
	5: '.....', 6: '-....', 7: '--...', 8: '---..', 9: '----.',
	'/': '-..-.', '?': '..--..', ',': '--..--', '.': '.-.-.-', '=': '-...-',
};

//	立ち上がり/立ち下がりを 5ms なまらせた、実機の受信音に近い CW を作る
function synthCW(text, { wpm, toneHz, snrDb, amp = 0.3 }) {
	const rate = ctx.sampleRate;
	const unit = 1.2 / wpm;
	const keys = [[false, 0.6]];
	let first = true;
	for (const raw of text.toUpperCase()) {
		if (raw === ' ') { keys.push([false, 7 * unit]); first = true; continue; }
		const code = MORSE[raw];
		if (!code) continue;
		if (!first) keys.push([false, 3 * unit]);
		first = false;
		code.split('').forEach((e, i) => {
			if (i > 0) keys.push([false, unit]);
			keys.push([true, e === '.' ? unit : 3 * unit]);
		});
	}
	keys.push([false, 1.0]);

	const n = Math.round(keys.reduce((a, [, d]) => a + d, 0) * rate);
	const buf = ctx.createBuffer(1, n, rate);
	const out = buf.getChannelData(0);
	const env = new Float32Array(n);
	let pos = 0;
	for (const [on, dur] of keys) {
		const len = Math.round(dur * rate);
		if (on) for (let i = 0; i < len && pos + i < n; i++) env[pos + i] = 1;
		pos += len;
	}
	const rn = Math.max(1, Math.round(0.005 * rate));
	const noise = amp / Math.pow(10, snrDb / 20);
	const w = 2 * Math.PI * toneHz / rate;
	let run = 0;
	for (let i = 0; i < rn; i++) run += env[Math.min(n - 1, i)];
	for (let i = 0; i < n; i++) {
		const add = env[Math.min(n - 1, i + rn)] ?? 0;
		const sub = env[Math.max(0, i - 1)] ?? 0;
		if (i > 0) run += add - sub;
		const e = Math.max(0, Math.min(1, run / rn));
		const u1 = Math.random() || 1e-9, u2 = Math.random();
		const g = Math.sqrt(-2 * Math.log(u1)) * Math.cos(2 * Math.PI * u2);
		out[i] = amp * e * Math.sin(w * i) + noise * g;
	}
	return buf;
}

//==================================================================
//	ワークレットからのフレーム受信
//==================================================================
const SR = 8192;                          // スコープ列のリング (JS 側)
const sMn = new Int16Array(SR), sMx = new Int16Array(SR);
const sMag = new Uint16Array(SR), sGate = new Uint8Array(SR);
let sTotal = 0;                           // 受け取った列の通し番号 (最新 = sTotal-1)

let ticker = [];                          // スコープ上に流す文字 {ch, col}
let chars = [];                           // デコード文字 (コードポイント列)
let status = new Uint32Array(18);
let spec = new Uint16Array(SPEC_BINS);
let lastClip = 0, clipMs = 0;
let inRate = 0;                           // 実効入力レート (正常なら 8000)
let inRateMs = 0, lastPushed = -1, lastT = 0;
let gotFrame = false;                     // 初フレーム前は HTML の初期表示のまま
let peakHold = 0, peakMs = 0;             // Peak 表示は実機と同じく 1 秒ホールド
let dirty = false;

function onWorkletMessage(e) {
	const m = e.data;
	if (m.type === 'ready') {
		ready = true;
		setState(`準備完了 (入力 ${m.rate}Hz → ${m.target}Hz)`);
		return;
	}
	if (m.type !== 'frame') return;

	status = m.status;
	spec = m.spec;

	// スコープ列をリングへ
	const n = m.status[ST.scopeN];
	const first = m.colFirst;
	const dv = new DataView(m.cols.buffer);
	for (let i = 0; i < n; i++) {
		const o = i * m.stride, k = (first + i) % SR;
		sMn[k] = dv.getInt16(o, true);
		sMx[k] = dv.getInt16(o + 2, true);
		sMag[k] = dv.getUint16(o + 4, true);
		sGate[k] = m.cols[o + 6];
	}
	if (n) sTotal = first + n;

	// デコード文字
	const nc = m.status[ST.charN];
	if (nc) {
		const cv = new DataView(m.chars.buffer);
		for (let i = 0; i < nc; i++) {
			const col = cv.getUint32(i * 8, true);
			const ch = m.chars[i * 8 + 4];
			putChar(ch);
			if (ch !== 0x20) ticker.push({ ch, col });
		}
		if (ticker.length > 96) ticker = ticker.slice(-96);
		renderText();
	}

	// 実効入力レート。フレーム間の増分から出す (累積平均だと入力が
	// 止まったあとズルズル下がって読めない)。8000 を割っていれば
	// オーディオスレッドが締め切りに間に合わず入力を落としている
	if (lastPushed >= 0 && m.t > lastT) {
		const d = m.pushed - lastPushed, dt = m.t - lastT;
		if (d > 0) {
			const r = d / dt;
			inRate = inRate ? inRate + (r - inRate) * 0.1 : r;   // EMA で平滑
			inRateMs = performance.now();
		}
	}
	lastPushed = m.pushed;
	lastT = m.t;

	// BPF のトーン追従はここで行う。描画 (requestAnimationFrame) は
	// タブやペインが隠れると止まるが、こちらはワークレットからの
	// フレームで動くので、裏に回っても追従が続く
	updateBpf();

	const clip = m.status[ST.clip];
	if (clip !== lastClip) { lastClip = clip; clipMs = performance.now(); }
	gotFrame = true;
	dirty = true;
}

//==================================================================
//	デコード文字エリア
//==================================================================
function putChar(byte) {
	const cp = codepointOf(byte);
	// 濁点/半濁点は直前の文字と合成 (カ + ゛→ ガ) — display.cpp と同じ規則
	if ((cp === 0x309B || cp === 0x309C) && chars.length) {
		const prev = chars[chars.length - 1];
		let comb = 0;
		if (cp === 0x309B && DAKUTEN_OK.has(prev)) comb = prev === 0x30A6 ? 0x30F4 : prev + 1;
		else if (cp === 0x309C && HANDAKUTEN_OK.has(prev)) comb = prev + 2;
		if (comb) { chars[chars.length - 1] = comb; return; }
	}
	chars.push(cp);
	if (chars.length > 4000) chars = chars.slice(-3000);
}

function renderText() {
	const atBottom = el.text.scrollHeight - el.text.scrollTop - el.text.clientHeight < 40;
	if (!chars.length) { tHead.textContent = ''; tNew.textContent = ''; return; }
	tHead.textContent = String.fromCodePoint(...chars.slice(0, -1));
	tNew.textContent = String.fromCodePoint(chars[chars.length - 1]);
	if (atBottom) el.text.scrollTop = el.text.scrollHeight;
}

//==================================================================
//	描画
//==================================================================
function fitCanvas(cv) {
	const dpr = Math.min(2, window.devicePixelRatio || 1);
	const w = Math.max(240, Math.round(cv.clientWidth));
	const h = Math.round(cv.clientHeight || 220);
	if (cv.width !== Math.round(w * dpr) || cv.height !== Math.round(h * dpr)) {
		cv.width = Math.round(w * dpr);
		cv.height = Math.round(h * dpr);
	}
	const g = cv.getContext('2d');
	g.setTransform(dpr, 0, 0, dpr, 0, 0);
	return { g, w, h };
}

//------------------------------------------------------------------
//	FFT スペクトラム (display.cpp の draw_fft_panel と同じ構成)
//------------------------------------------------------------------
const EQ_BIN_START = 10, EQ_BAR_COUNT = 29;     // bin10(312.5Hz)〜bin38(1187.5Hz)
const EQ_F_MIN = EQ_BIN_START * HZ_PER_BIN;
const EQ_F_MAX = (EQ_BIN_START + EQ_BAR_COUNT - 1) * HZ_PER_BIN;
let dispMax = 8000, bar = new Float32Array(EQ_BAR_COUNT);
let peakPx = new Float32Array(EQ_BAR_COUNT), peakTtl = new Uint8Array(EQ_BAR_COUNT);

//	単調3次補間 (Fritsch-Carlson)。FFT は 29 点しか無いのに実機の 3 倍以上の
//	幅へ引き伸ばすので、直線で結ぶと折れ線が目立つ。これは「点と点の間を
//	滑らかにつなぐ」だけで分解能が上がるわけではないが、データに無い山や谷を
//	作らないので、スペクトルの読みを歪めない
//	(Catmull-Rom はオーバーシュートして偽のピークを描くので使わない)
function smoothCurve(ys, outN) {
	const n = ys.length;
	if (n < 3) return Float64Array.from(ys);

	const d = new Float64Array(n - 1);          // 区間の傾き
	for (let i = 0; i < n - 1; i++) d[i] = ys[i + 1] - ys[i];
	const m = new Float64Array(n);              // 各点の接線
	m[0] = d[0];
	m[n - 1] = d[n - 2];
	for (let i = 1; i < n - 1; i++) {
		m[i] = (d[i - 1] * d[i] <= 0) ? 0 : (d[i - 1] + d[i]) / 2;
	}
	for (let i = 0; i < n - 1; i++) {           // 単調性の保証
		if (d[i] === 0) { m[i] = 0; m[i + 1] = 0; continue; }
		const a = m[i] / d[i], b = m[i + 1] / d[i];
		const ss = a * a + b * b;
		if (ss > 9) {
			const t = 3 / Math.sqrt(ss);
			m[i] = t * a * d[i];
			m[i + 1] = t * b * d[i];
		}
	}

	const out = new Float64Array(outN);
	for (let k = 0; k < outN; k++) {
		const u = (k / (outN - 1)) * (n - 1);
		let i = Math.min(n - 2, Math.floor(u));
		const t = u - i, t2 = t * t, t3 = t2 * t;
		out[k] = (2 * t3 - 3 * t2 + 1) * ys[i] + (t3 - 2 * t2 + t) * m[i]
		       + (-2 * t3 + 3 * t2) * ys[i + 1] + (t3 - t2) * m[i + 1];
	}
	return out;
}

function hzToX(hz, x0, plotW) {
	return x0 + ((hz - EQ_F_MIN) / (EQ_F_MAX - EQ_F_MIN)) * plotW;
}
function xToHz(x, x0, plotW) {
	return EQ_F_MIN + ((x - x0) / plotW) * (EQ_F_MAX - EQ_F_MIN);
}

function drawFFT() {
	const { g, w, h } = fitCanvas(el.fft);
	const x0 = 10, plotW = w - 20;
	const top = 12, baseY = h - 26, plotH = baseY - top;

	g.fillStyle = C.panelBg;
	g.fillRect(0, 0, w, h);
	g.strokeStyle = C.frame;
	g.lineWidth = 1;
	g.strokeRect(0.5, 0.5, w - 1, h - 1);

	// TONE 選択レンジ (600〜1000Hz) のガイド帯
	const xLo = hzToX(600, x0, plotW), xHi = hzToX(1000, x0, plotW);
	g.fillStyle = C.band;
	g.fillRect(xLo, top - 4, xHi - xLo, plotH + 8);
	g.strokeStyle = C.marker;
	g.beginPath(); g.moveTo(xLo, top - 4); g.lineTo(xLo, baseY + 4);
	g.moveTo(xHi, top - 4); g.lineTo(xHi, baseY + 4); g.stroke();

	// 選択中トーンの検出帯域 (Goertzel 1 ビン幅、WPM 追従で ±34/±68Hz)
	const toneHz = status[ST.toneHz] || 600;
	const halfBw = (status[ST.gateBw] || 67) / 2;
	const xc = hzToX(toneHz, x0, plotW);
	const xbl = hzToX(toneHz - halfBw, x0, plotW), xbr = hzToX(toneHz + halfBw, x0, plotW);
	g.fillStyle = C.toneBand;
	g.fillRect(xbl, top - 4, xbr - xbl, plotH + 8);
	g.strokeStyle = C.env;
	g.beginPath(); g.moveTo(xbl, top - 4); g.lineTo(xbl, baseY + 4);
	g.moveTo(xbr, top - 4); g.lineTo(xbr, baseY + 4); g.stroke();

	// AGC (表示帯域のフレーム最大値にゆっくり追従)
	let fmax = 0;
	for (let b = 0; b < EQ_BAR_COUNT; b++) fmax = Math.max(fmax, spec[EQ_BIN_START + b] || 0);
	dispMax += (fmax * 1.15 - dispMax) * 0.05;
	if (dispMax < 8000) dispMax = 8000;

	const yPt = new Float32Array(EQ_BAR_COUNT);
	for (let b = 0; b < EQ_BAR_COUNT; b++) {
		let t = ((spec[EQ_BIN_START + b] || 0) / dispMax) * plotH;
		if (t > plotH) t = plotH;
		bar[b] += (t - bar[b]) * (t >= bar[b] ? 0.7 : 0.35);
		yPt[b] = baseY - bar[b];
		if (bar[b] >= peakPx[b]) { peakPx[b] = bar[b]; peakTtl[b] = 25; }
		else if (peakTtl[b] > 0) peakTtl[b]--;
		else if (peakPx[b] > 0) peakPx[b] -= plotH / 42;
	}

	const bx = (b) => x0 + (b / (EQ_BAR_COUNT - 1)) * plotW;
	// 点と点の間を単調3次補間で埋める (2px 刻み)。実機は 5px/点なので
	// 直線結合でも気にならないが、Web では 16px/点まで開くため
	const curve = smoothCurve(yPt, Math.max(EQ_BAR_COUNT, Math.round(plotW / 2) + 1));
	const cx = (i) => x0 + (i / (curve.length - 1)) * plotW;

	// 塗り
	g.fillStyle = C.eqFill;
	g.beginPath();
	g.moveTo(cx(0), baseY);
	for (let i = 0; i < curve.length; i++) g.lineTo(cx(i), curve[i]);
	g.lineTo(cx(curve.length - 1), baseY);
	g.closePath(); g.fill();
	// エンベロープライン
	g.strokeStyle = C.eqLine; g.lineWidth = 1.5;
	g.beginPath();
	for (let i = 0; i < curve.length; i++) (i ? g.lineTo : g.moveTo).call(g, cx(i), curve[i]);
	g.stroke();
	// ピークホールド
	g.strokeStyle = C.eqPeak; g.lineWidth = 1;
	g.beginPath();
	for (let b = 0; b < EQ_BAR_COUNT; b++) {
		if (peakPx[b] <= 1) continue;
		const y = Math.round(baseY - peakPx[b]) + 0.5;
		g.moveTo(bx(b) - 3, y); g.lineTo(bx(b) + 3, y);
	}
	g.stroke();

	// 選択中トーンの中心マーカー (下端の三角)
	g.fillStyle = C.env;
	g.beginPath();
	g.moveTo(xc, baseY + 3); g.lineTo(xc - 4, baseY + 10); g.lineTo(xc + 4, baseY + 10);
	g.closePath(); g.fill();

	// 周波数目盛
	g.fillStyle = C.label;
	g.font = '10px ui-monospace, monospace';
	g.textAlign = 'center';
	for (const f of [400, 600, 800, 1000, 1200]) {
		if (f < EQ_F_MIN || f > EQ_F_MAX) continue;
		g.fillText(f >= 1000 ? `${f / 1000}k` : String(f), hzToX(f, x0, plotW), h - 8);
	}
	g.textAlign = 'left';
}

//------------------------------------------------------------------
//	オシロスコープ (display.cpp の draw_scope_panel と同じ構成)
//------------------------------------------------------------------
let showKey = true, showEnv = true, showRaw = true;
let rawMax = 100, envMax = 500;
let scopeZoom = 2;                        // 時間軸の倍率 (1〜3)

function drawScope() {
	const { g, w, h } = fitCanvas(el.scope);
	g.fillStyle = C.panelBg;
	g.fillRect(0, 0, w, h);
	g.strokeStyle = C.frame; g.lineWidth = 1;
	g.strokeRect(0.5, 0.5, w - 1, h - 1);

	const x0 = 4;
	// 時間軸の倍率。1 倍で 1 列 = 1px (実機と同じ密度)、上げると 1 列を
	// 太く描くので細かい符号が見えるようになる (そのぶん見える時間は減る)。
	// 列の左端を整数へ丸めて幅を差分で出し、隙間も重なりも出さない
	const zoom = scopeZoom;
	const cols = Math.max(16, Math.floor((w - 8) / zoom));
	const start = Math.max(0, sTotal - cols);
	const colL = (i) => Math.round(x0 + (i - start) * zoom);
	const colW = (i) => Math.max(1, colL(i + 1) - colL(i));
	const colC = (i) => colL(i) + colW(i) / 2;        // 列の中心 (折れ線・文字用)
	const textH = 22, keyY = textH + 4, waveTop = keyY + 10, waveBot = h - 8;
	const midY = (waveTop + waveBot) / 2, halfH = (waveBot - waveTop) / 2;

	// AGC (生波形 / エンベロープ別。実機と同じ時定数)
	let rmax = 0, emax = 0;
	for (let i = start; i < sTotal; i++) {
		const k = i % SR;
		rmax = Math.max(rmax, Math.abs(sMx[k]), Math.abs(sMn[k]));
		emax = Math.max(emax, sMag[k]);
	}
	rawMax += (rmax * 1.1 - rawMax) * 0.1;
	if (rawMax < 60) rawMax = 60;
	envMax += (emax * 1.1 - envMax) * 0.1;
	if (envMax < 400) envMax = 400;

	if (showRaw) {
		g.strokeStyle = '#142837';
		g.beginPath(); g.moveTo(x0, midY + 0.5); g.lineTo(x0 + cols * zoom, midY + 0.5); g.stroke();
	}

	// 生波形 min/max バンド
	if (showRaw) {
		g.fillStyle = C.raw;
		for (let i = start; i < sTotal; i++) {
			const k = i % SR;
			let y1 = midY - (sMx[k] / rawMax) * halfH;
			let y2 = midY - (sMn[k] / rawMax) * halfH;
			y1 = Math.max(waveTop, y1); y2 = Math.min(waveBot, y2);
			if (y2 < y1) [y1, y2] = [y2, y1];
			g.fillRect(colL(i), y1, colW(i), y2 - y1 + 1);
		}
	}

	// エンベロープ (下端基準の折れ線)
	if (showEnv) {
		g.strokeStyle = C.env; g.lineWidth = 1.2;
		g.beginPath();
		let started = false;
		for (let i = start; i < sTotal; i++) {
			const k = i % SR, x = colC(i);
			const eh = Math.min(waveBot - waveTop, (sMag[k] / envMax) * (waveBot - waveTop));
			const y = waveBot - eh;
			if (!started) { g.moveTo(x, y); started = true; } else g.lineTo(x, y);
		}
		g.stroke();
	}

	// キー判定バー
	if (showKey) {
		g.fillStyle = C.gate;
		for (let i = start; i < sTotal; i++) {
			if (!sGate[i % SR]) continue;
			g.fillRect(colL(i), keyY, colW(i), 4);
		}
		// デコード文字を符号区間の中央に置き、掃引とともに左へ流す
		g.fillStyle = C.textNew;
		g.font = '15px "Hiragino Sans", "Noto Sans JP", sans-serif';
		g.textAlign = 'center';
		for (const t of ticker) {
			if (t.col < start || t.col >= sTotal) continue;
			g.fillText(String.fromCodePoint(codepointOf(t.ch)), colC(t.col), textH - 4);
		}
		g.textAlign = 'left';
	}

	// 掃引レート (実機の scope_col_ms_x10 と同じ値)
	// 掃引レートはキャンバス上だと文字とぶつかるのでキャプションに出す
	const ms = status[ST.colMsX10] / 10;
	$('scopeSpan').textContent = ms > 0
		? `${(cols * ms / 1000).toFixed(1)}s span / ${ms.toFixed(1)}ms per col`
		+ (zoom !== 1 ? ` / x${zoom.toFixed(1)}` : '')
		: '';
}

//------------------------------------------------------------------
//	ステータス行
//------------------------------------------------------------------
function drawStatus() {
	// 中核から 1 フレームも届く前は全部 0 なので、HTML の初期表示 (US / AUTO) を保つ
	if (!gotFrame) return;

	el.wpm.textContent = status[ST.wpm] || '--';
	el.toneHz.textContent = status[ST.toneHz] || '---';
	// Peak は符号の切れ目で 0 に戻るので、実機の draw_fft_panel と同じく
	// 最後に検出した値を 1 秒保持する (そうしないと点滅して読めない)
	const now = performance.now();
	if (status[ST.peakHz]) { peakHold = status[ST.peakHz]; peakMs = now; }
	el.peakHz.textContent = (peakHold && now - peakMs < 1000) ? `${peakHold}Hz` : '----';
	el.btnMode.textContent = status[ST.mode] ? 'JP' : 'US';
	el.btnMode.className = status[ST.mode] ? 'jp' : 'on';
	el.btnTone.textContent = status[ST.toneAuto] ? 'AUTO' : `${status[ST.toneHz]}Hz`;
	el.btnTone.style.color = status[ST.toneAuto] ? C.gate : '';

	// 入力レベル (dBFS)。0 dBFS = フルスケール (実機では ADC 半スイング)
	const pk = status[ST.inputPeak];
	const db = pk > 0 ? 20 * Math.log10(pk / 2048) : -60;
	const shown = Math.max(-50, Math.min(0, db));
	el.lvlBar.style.width = `${((shown + 50) / 50) * 100}%`;
	el.lvlBar.style.background = db >= -2.5 ? C.lvlHi : db >= -5.2 ? C.lvlMid : C.lvlLo;
	el.lvlTx.textContent = pk > 0 ? `${db.toFixed(1)} dBFS` : '-inf dBFS';
	el.clip.style.visibility = performance.now() - clipMs < 500 && clipMs ? 'visible' : 'hidden';

	// 取りこぼしていると WPM 推定も符号長も狂うので、ずれたら赤で出す。
	// 入力が止まったら表示も消す (止まったまま古い値を出さない)
	if (performance.now() - inRateMs > 1500) { el.inRate.textContent = ''; inRate = 0; }
	else if (inRate > 0) {
		const off = (inRate / 8000 - 1) * 100;
		el.inRate.textContent = `${inRate.toFixed(0)} sps`;
		el.inRate.style.color = Math.abs(off) > 0.5 ? '#ff9a8f' : 'var(--dim)';
		el.inRate.title = `実効入力レート (正常 8000 sps、ずれ ${off.toFixed(2)}%)`;
	}
}

function frame() {
	dirty = false;
	drawStatus();
	if (filePlaying) fileUpdateUI();
	drawFFT();
	drawScope();
	requestAnimationFrame(frame);
}
requestAnimationFrame(frame);

//==================================================================
//	操作
//==================================================================
document.querySelectorAll('[role=tab]').forEach((b) => {
	b.onclick = () => {
		document.querySelectorAll('[role=tab]').forEach((x) => x.setAttribute('aria-selected', x === b));
		document.querySelectorAll('.srcpane').forEach((p) => p.classList.remove('active'));
		$(`pane-${b.dataset.src}`).classList.add('active');
	};
});

const guard = (fn) => async (...a) => {
	try { await fn(...a); }
	catch (e) { setState(`${e.name}: ${e.message}`, true); }
};

$('micStart').onclick = guard(startMic);
// チャンネル切替と BPF の ON/OFF は再生中でもその場で繋ぎ替える (止めずに比べられる)
$('chSel').onchange = () => wireChain();
$('bpfOn').onchange = () => wireChain();
$('bpfCenter').onchange = () => updateBpf();
$('bpfQ').oninput = () => { $('bpfQV').textContent = (+$('bpfQ').value).toFixed(1); updateBpf(); };
$('micRescan').onclick = guard(async () => { await ensureEngine(); await listMics(); });
// モニター音はどのタブのチェックでも即座に反映される (再生中でも)
for (const id of ['micMonitor', 'tabMonitor', 'fileMonitor', 'testMonitor']) {
	$(id).onchange = (e) => applyMonitor(e.target.checked);
}
$('tabStart').onclick = guard(startTab);

$('fileInput').onchange = guard((e) => e.target.files[0] && loadFile(e.target.files[0]));
$('filePlay').onclick = guard(filePlayPause);
$('fileHome').onclick = guard(() => fileSeek(0));
$('fileBack').onclick = guard(() => fileSeek(fileNow() - SEEK_STEP));
$('fileFwd').onclick  = guard(() => fileSeek(fileNow() + SEEK_STEP));
$('fileEnd').onclick  = guard(() => fileSeek(fileBuffer ? fileBuffer.duration : 0));

// シークバー: ドラッグ中は表示の追従を止め、離した時点で飛ぶ
$('fileSeek').oninput = () => {
	fileSeeking = true;
	if (fileBuffer) {
		const t = (+$('fileSeek').value / 1000) * fileBuffer.duration;
		$('fileTime').textContent = `${mmss(t)} / ${mmss(fileBuffer.duration)}`;
	}
};
$('fileSeek').onchange = guard(async () => {
	fileSeeking = false;
	if (fileBuffer) await fileSeek((+$('fileSeek').value / 1000) * fileBuffer.duration);
});

for (const [r, v] of [['tWpm', 'tWpmV'], ['tHz', 'tHzV'], ['tSnr', 'tSnrV']]) {
	$(r).oninput = () => { $(v).textContent = $(r).value; };
}
$('testPlay').onclick = guard(async () => {
	await ensureEngine();
	const buf = synthCW($('tText').value, {
		wpm: +$('tWpm').value, toneHz: +$('tHz').value, snrDb: +$('tSnr').value,
	});
	playBuffer(buf, $('testMonitor').checked, () => {
		drainDecoder();                 // 末尾の文字を取りこぼさない
		$('testStop').disabled = true;
		$('testPlay').disabled = false;
	});
	$('testStop').disabled = false;
	$('testPlay').disabled = true;
	setState(`テスト信号: ${$('tWpm').value}WPM / ${$('tHz').value}Hz / S:N ${$('tSnr').value}dB`);
});
$('testStop').onclick = () => { drainDecoder(); stopSource(); setState('停止中'); };
$('stopAll').onclick = () => {
	drainDecoder();
	filePlaying = false;
	stopSource();
	fileUpdateUI();
	setState('停止中');
};

el.gain.oninput = () => setGain(+el.gain.value);
$('gainReset').onclick = () => setGain(0);

el.btnMode.onclick = () => node?.port.postMessage({ type: 'toggleMode' });
el.btnTone.onclick = () => node?.port.postMessage({ type: 'cycleTone' });

//==================================================================
//	パネルの全幅表示
//	オシロは掃引が WPM 追従で細かいので、横幅いっぱいにすると
//	見える時間が倍以上になる (1 列 = 1px のため幅がそのまま時間になる)
//==================================================================
let expanded = null;                      // null | 'fft' | 'scope'

function setExpanded(which) {
	expanded = (expanded === which) ? null : which;
	const fftCard = $('panelFft'), scopeCard = $('panelScope');
	fftCard.classList.toggle('wide', expanded === 'fft');
	fftCard.classList.toggle('hidden', expanded === 'scope');
	scopeCard.classList.toggle('wide', expanded === 'scope');
	scopeCard.classList.toggle('hidden', expanded === 'fft');
}

// 見出し行はどちらもクリックで切替 (KEY/ENV/RAW ボタンの上は除く)
for (const [capId, which] of [['capFft', 'fft'], ['capScope', 'scope']]) {
	$(capId).onclick = (e) => {
		// 見出し内のボタンやスライダーの操作は全幅切替に拾わせない
		if (!e.target.closest('button, input, select, label')) setExpanded(which);
	};
}

// オシロは波形部分のクリックで切替 (他の用途が無いので単クリック)
el.scope.onclick = () => setExpanded('scope');

// FFT はクリックがトーン選択に使われている (実機の「FFT パネル内タップ」と
// 同じ機能) ので、全幅切替はダブルクリックに割り当てる。
// ダブルクリックの 1 打目でトーンが動かないよう、選択は 250ms 遅らせて
// ダブルクリックが来たら取り消す
let fftClickTimer = null;

el.fft.onclick = (e) => {
	if (fftClickTimer) return;
	const r = el.fft.getBoundingClientRect();
	const x = e.clientX - r.left, w = r.width;
	fftClickTimer = setTimeout(() => {
		fftClickTimer = null;
		node?.port.postMessage({ type: 'toneHz', hz: Math.round(xToHz(x, 10, w - 20)) });
	}, 250);
};

el.fft.ondblclick = () => {
	clearTimeout(fftClickTimer);
	fftClickTimer = null;
	setExpanded('fft');
};

$('scopeZoom').oninput = () => {
	scopeZoom = +$('scopeZoom').value;
	$('scopeZoomV').textContent = scopeZoom.toFixed(1);
};

$('bKey').onclick = (e) => { showKey = !showKey; e.target.className = `trace ${showKey ? 'on' : 'off'}`; };
$('bEnv').onclick = (e) => { showEnv = !showEnv; e.target.className = `trace ${showEnv ? 'on' : 'off'}`; };
$('bRaw').onclick = (e) => { showRaw = !showRaw; e.target.className = `trace ${showRaw ? 'on' : 'off'}`; };

$('btnCopy').onclick = async () => {
	await navigator.clipboard.writeText(String.fromCodePoint(...chars));
	$('btnCopy').textContent = 'コピーしました';
	setTimeout(() => { $('btnCopy').textContent = 'コピー'; }, 1200);
};
$('btnClear').onclick = () => { chars = []; ticker = []; renderText(); };

// 起動時にデバイス一覧を出せるだけ出しておく (名称は許可後に埋まる)
if (navigator.mediaDevices?.enumerateDevices) listMics().catch(() => {});
navigator.mediaDevices?.addEventListener?.('devicechange', () => listMics().catch(() => {}));
