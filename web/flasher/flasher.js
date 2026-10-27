//
//	CW Decoder (ESP32 版) の USB 書き込みツール
//
//	Web Serial API + esptool-js (Espressif 公式、vendor/ に同梱) で、過去の版を
//	含めて USB 経由で書き込む。OTA (本体の自動更新 / SETUP の FW更新) とは別に、
//	ダウングレードや設定 (NVS) の初期化、OTA が使えない状態からの復旧に使う。
//
//	書き込む内容は ../firmware/versions.json (tools/make_versions.py が作る):
//	  0x1000  bootloader.bin
//	  0x8000  partitions.bin
//	  0xE000  boot_app0.bin   起動先を app0 に戻す。OTA で更新した本体は app1 から
//	                          起動しているので、これを書かないと app0 を書き換えても
//	                          古いファームが起動し続ける
//	  0x10000 firmware_<パネル>.bin
//	「設定を初期化する」は NVS (0x9000〜0xDFFF) を 0xFF で埋める (消去状態と同じ)。
//
//	接続したときと書き込んだ後だけ、ポートを開いて本体を再起動させ、起動ログの
//	[boot] / [lcd] 行からいまの版とパネルを読んだら閉じる (画面にログは出さない)。
//	開きっぱなしにしないのは、本体がスコープログを 115200bps いっぱいに流し
//	続けるため。書き込みは閉じたポートを esptool-js に渡して行う。
//
import { ESPLoader, Transport } from './vendor/esptool-js-0.6.1/bundle.js';
import { md5hex } from './md5.js';

const $ = (id) => document.getElementById(id);

// CYD 系の基板に載る USB シリアル変換 (CH340 / CP210x / FTDI) と ESP 内蔵 USB
const PORT_FILTERS = [
	{ usbVendorId: 0x1a86 }, { usbVendorId: 0x10c4 },
	{ usbVendorId: 0x0403 }, { usbVendorId: 0x303a },
];
const USB_NAMES = { 0x1a86: 'CH340', 0x10c4: 'CP210x', 0x0403: 'FTDI', 0x303a: 'ESP USB' };
// この基板の CH340 は 921600bps だと同期が乱れて書けなかった (実測)。
// pio の既定と同じ 460800 で書き、失敗したら 115200 でやり直す
const FLASH_BAUDS = [460800, 115200];
const LOG_BAUD = 115200;
const BOOT_WAIT_MS = 5000;       // 再起動から起動ログの [boot] 行が出るまで待つ時間

let index = null;                // versions.json
let port = null;
let busy = false;                // 書き込み中
let reading = false;             // 起動ログを読み取り中
let device = { version: null, panel: null };

//==================================================================
//	表示
//==================================================================
function setState() {
	const el = $('state');
	if (!port) {
		el.innerHTML = '<span class="dim">本体を USB で接続して「ポートを選んで接続」を押してください。</span>';
		return;
	}
	if (reading) {
		el.innerHTML = '<span class="dim">本体を再起動して、いまの版とパネルを読み取っています…</span>';
		return;
	}
	const v = device.version ? `v${device.version}` : '<span class="dim">不明</span>';
	const p = device.panel || '<span class="dim">不明</span>';
	el.innerHTML = `本体のファーム: <b>${v}</b>&nbsp;&nbsp;パネル: <b>${p}</b>` +
		(device.version ? '' : ' <span class="dim">(読み取れませんでした。書き込みはできます。パネルを選んでください)</span>');
}

function updateButtons() {
	const connected = !!port;
	$('btnConnect').disabled = connected || busy || reading || !('serial' in navigator);
	$('btnDisconnect').disabled = !connected || busy || reading;
	$('btnFlash').disabled = !connected || busy || reading || !index;
	$('selVer').disabled = busy || !index;
	for (const r of document.querySelectorAll('input[name=panel], input[name=erase]')) r.disabled = busy;
}

function flog(s, cls) {
	const pre = $('flog');
	const line = document.createElement('div');
	line.textContent = s;
	if (cls) line.className = cls;
	pre.appendChild(line);
	pre.scrollTop = pre.scrollHeight;
}

function flashMsg(s, cls = 'dim') {
	// 一番下のステータス欄に出す。長いと省略されるので、全文はポインタを当てると出る
	$('flashMsg').className = cls;
	$('flashMsg').textContent = s;
	$('flashMsg').title = s;
}

function progress(frac) {
	const p = Math.max(0, Math.min(1, frac));
	$('barFill').style.width = `${(p * 100).toFixed(1)}%`;
	$('pct').textContent = `${Math.round(p * 100)}%`;
}

const vkey = (v) => v.split('.').map(Number);
function vcmp(a, b) {
	const x = vkey(a), y = vkey(b);
	for (let i = 0; i < Math.max(x.length, y.length); i++) {
		const d = (x[i] || 0) - (y[i] || 0);
		if (d) return d;
	}
	return 0;
}

//==================================================================
//	版の一覧
//==================================================================
async function loadIndex() {
	try {
		const r = await fetch('../firmware/versions.json', { cache: 'no-cache' });
		if (!r.ok) throw new Error(`HTTP ${r.status}`);
		index = await r.json();
	} catch (e) {
		$('selVer').innerHTML = '<option>版の一覧を読めません</option>';
		flashMsg(`版の一覧 (firmware/versions.json) を読めません: ${e.message}`, 'err');
		return;
	}
	// 見出しの版表示は Web 版デコーダ (FW_VERSION を手書き) と違い、配布中の
	// 最新版から取る。リリースのたびに書き換えなくても食い違わない
	$('toolVer').textContent = index.latest ? `v${index.latest}` : '';
	const sel = $('selVer');
	sel.innerHTML = '';
	for (const v of index.versions) {
		const o = document.createElement('option');
		o.value = v.version;
		o.textContent = `v${v.version}` + (v.date ? `  (${v.date})` : '') +
			(v.version === index.latest ? '  最新' : '');
		sel.appendChild(o);
	}
	sel.value = index.latest;
	showVersion();
	updateButtons();
}

function selected() {
	return index?.versions.find((v) => v.version === $('selVer').value) || null;
}

function showVersion() {
	const v = selected();
	const ul = $('notes');
	ul.innerHTML = '';
	for (const n of v?.notes || []) {
		const li = document.createElement('li');
		li.textContent = n;
		ul.appendChild(li);
	}
	let w = '';
	if (v && device.version && vcmp(v.version, device.version) < 0) {
		w = `本体 (v${device.version}) より古い版です (ダウングレード)`;
	}
	$('verWarn').textContent = w;
	checkPanel();
}

function chosenPanel() {
	return document.querySelector('input[name=panel]:checked')?.value || null;
}

function checkPanel() {
	const p = chosenPanel();
	let h = '';
	if (device.panel && p && p !== device.panel) {
		h = `⚠ 本体は ${device.panel} です。違うパネル用を書くと表示が上下逆になります`;
		$('panelHint').className = 'warn';
	} else if (device.panel) {
		h = '(本体の起動ログから自動選択)';
		$('panelHint').className = 'dim';
	}
	$('panelHint').textContent = h;
}

//==================================================================
//	起動ログの読み取り
//==================================================================
function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

//	本体をリセットして起動ログを出させる (EN を 100ms 落とす、IO0 は High のまま)。
//	DTR / RTS は基板の自動リセット回路につながっている (RTS = EN、DTR = IO0)
async function resetDevice() {
	await port.setSignals({ dataTerminalReady: false, requestToSend: true });
	await sleep(100);
	await port.setSignals({ requestToSend: false });
}

//	ポートを開いて本体を再起動し、起動ログから版とパネルを読んで閉じる。
//	[boot] 行が BOOT_WAIT_MS 以内に出なければもう 1 回だけ再起動する。
//	起動ログの例:
//	  [lcd] panel = ST7789
//	  [boot] v3.4 build Oct 24 2026 10:27:01  running=app0 ...
async function readBootInfo() {
	const info = { version: null, panel: null };
	// 受信バッファは既定 255 バイトしかなく、スコープログであふれるので広げる
	await port.open({ baudRate: LOG_BAUD, bufferSize: 1 << 16 });
	try {
		await port.setSignals({ dataTerminalReady: false, requestToSend: false });
		for (let attempt = 0; attempt < 2 && !info.version; attempt++) {
			await resetDevice();
			const deadline = Date.now() + BOOT_WAIT_MS;
			const dec = new TextDecoder('utf-8');
			let partial = '';
			while (!info.version && Date.now() < deadline && port.readable) {
				const reader = port.readable.getReader();
				const timer = setTimeout(() => reader.cancel().catch(() => {}), deadline - Date.now());
				try {
					for (;;) {
						const { value, done } = await reader.read();
						if (done) break;
						const parts = (partial + dec.decode(value, { stream: true })).split(/\r?\n/);
						partial = parts.pop();
						for (const line of parts) {
							let m = line.match(/^\[lcd\] panel = (ST7789|ILI9341)/);
							if (m) info.panel = m[1];
							m = line.match(/^\[boot\] v(\d+(?:\.\d+)+)/);
							if (m) info.version = m[1];
						}
						if (info.version) break;      // [lcd] は [boot] より先に出る
					}
				} catch {
					// あふれ・フレーミングエラー等。期限まで読み直す
				} finally {
					clearTimeout(timer);
					reader.releaseLock();
				}
			}
		}
	} finally {
		try { await port.close(); } catch { /* 抜かれた */ }
	}
	return info;
}

//	接続時の読み取り: 結果を画面に反映し、パネルの選択を本体に合わせる
async function detectDevice() {
	reading = true;
	setState();
	updateButtons();
	try {
		device = await readBootInfo();
	} finally {
		reading = false;
	}
	// 読み取れなかったときは ILI9341 を既定にする
	document.querySelector(`input[name=panel][value=${device.panel || 'ILI9341'}]`).checked = true;
	setState();
	showVersion();
	updateButtons();
}

//==================================================================
//	接続 / 切断
//==================================================================
async function connect() {
	let p;
	try {
		p = await navigator.serial.requestPort($('allPorts').checked ? {} : { filters: PORT_FILTERS });
	} catch {
		return;                  // 選択をキャンセルした
	}
	port = p;
	device = { version: null, panel: null };
	const info = port.getInfo();
	const vid = info.usbVendorId;
	$('portInfo').textContent = vid !== undefined
		? `${USB_NAMES[vid] || 'USB シリアル'} (${vid.toString(16).padStart(4, '0')}:${(info.usbProductId ?? 0).toString(16).padStart(4, '0')})`
		: '';
	setState();
	try {
		await detectDevice();
	} catch (e) {
		flashMsg(`ポートを開けません: ${e.message} — ほかのソフトが使っていないか確認してください`, 'err');
		port = null;
		reading = false;
		setState();
	}
	updateButtons();
}

async function disconnect() {
	port = null;
	device = { version: null, panel: null };
	$('portInfo').textContent = '';
	setState();
	showVersion();
	updateButtons();
}

//==================================================================
//	書き込み
//==================================================================
async function fetchChecked(entry) {
	const r = await fetch(`../firmware/${entry.path}`, { cache: 'no-cache' });
	if (!r.ok) throw new Error(`${entry.path} を取得できません (HTTP ${r.status})`);
	const data = new Uint8Array(await r.arrayBuffer());
	if (data.length !== entry.size || md5hex(data) !== entry.md5) {
		throw new Error(`${entry.path} が壊れています (サイズ / MD5 不一致)`);
	}
	return data;
}

function terminal() {
	let cur = '';
	return {
		clean() { },
		write(s) { cur += s; },
		writeLine(s) { flog(cur + s); cur = ''; },
	};
}

async function flashOnce(files, eraseAll, baud) {
	const transport = new Transport(port, false);
	const loader = new ESPLoader({ transport, baudrate: baud, terminal: terminal(), debugLogging: false });
	try {
		flashMsg('本体に接続中… (書き込みモードへ切り替えています)');
		await loader.main('default_reset');
		if (loader.chip.CHIP_NAME !== 'ESP32') {
			throw new Error(`このツールは ESP32 用です (接続されたのは ${loader.chip.CHIP_NAME})`);
		}
		if (eraseAll) {
			flashMsg('フラッシュ全体を消去中… (20 秒ほどかかります)');
			progress(0);
			await loader.eraseFlash();
		}
		const total = files.reduce((a, f) => a + f.data.length, 0);
		const before = files.map((_, i) => files.slice(0, i).reduce((a, f) => a + f.data.length, 0));
		flashMsg(`書き込み中… (${baud}bps)`);
		await loader.writeFlash({
			fileArray: files,
			flashMode: 'keep', flashFreq: 'keep', flashSize: 'keep',
			eraseAll: false, compress: true,
			reportProgress: (i, written, size) => {
				progress((before[i] + written / size * files[i].data.length) / total);
			},
			calculateMD5Hash: md5hex,    // 書き込み後に本体のフラッシュと照合する
		});
		progress(1);
		flashMsg('書き込み完了。本体を再起動しています…');
		await loader.after('hard_reset');
	} finally {
		try { await transport.disconnect(); } catch { /* 既に閉じている */ }
	}
}

async function flash() {
	const v = selected();
	const panel = chosenPanel();
	const erase = document.querySelector('input[name=erase]:checked').value;
	if (!v) return;
	if (!panel) {
		flashMsg('パネル (ST7789 / ILI9341) を選んでください', 'err');
		return;
	}
	if (!v.panels[panel]) {
		flashMsg(`v${v.version} には ${panel} 用がありません`, 'err');
		return;
	}
	const lines = [`v${v.version} (${panel}) を書き込みます。`];
	if (device.version) lines.push(`いまの本体: v${device.version}` + (device.panel ? ` / ${device.panel}` : ''));
	if (device.panel && device.panel !== panel) lines.push(`\n⚠ 本体は ${device.panel} です。違うパネル用を書くと表示が上下逆になります。`);
	if (device.version && vcmp(v.version, device.version) < 0) lines.push('\n古い版への書き戻し (ダウングレード) です。');
	if (erase === 'nvs') lines.push('\n設定 (タッチ補正・WiFi・時計の地域・NR など) をすべて初期化します。');
	if (erase === 'all') lines.push('\nフラッシュ全体を消去します。設定もすべて消えます。');
	lines.push('\n書き込み中は USB を抜かないでください。');
	if (!confirm(lines.join('\n'))) return;

	busy = true;
	updateButtons();
	$('flog').textContent = '';
	progress(0);
	const onLeave = (e) => { e.preventDefault(); e.returnValue = ''; };
	window.addEventListener('beforeunload', onLeave);
	try {
		flashMsg('ファイルを取得中…');
		const L = index.layout;
		const files = [
			{ address: L.bootloader, data: await fetchChecked(v.boot.bootloader) },
			{ address: L.partitions, data: await fetchChecked(v.boot.partitions) },
		];
		if (erase === 'nvs') {
			// 0xFF で埋める = 消去状態。NVS は空として扱われ、すべて既定値で起動する
			files.push({ address: L.nvs[0], data: new Uint8Array(L.nvs[1]).fill(0xff) });
		}
		files.push({ address: L.boot_app0, data: await fetchChecked(v.boot.boot_app0) });
		files.push({ address: L.app, data: await fetchChecked(v.panels[panel]) });
		flog(`取得: ${files.map((f) => `0x${f.address.toString(16)} ${f.data.length}B`).join(' / ')}`);

		let lastErr = null;
		for (const baud of FLASH_BAUDS) {
			try {
				await flashOnce(files, erase === 'all', baud);
				lastErr = null;
				break;
			} catch (e) {
				lastErr = e;
				flog(`失敗 (${baud}bps): ${e.message}`, 'err');
				if (/ESP32 用です/.test(e.message)) break;
				await sleep(500);
			}
		}
		if (lastErr) throw lastErr;

		// 起動ログで書いた版になったかを確かめる
		flashMsg('書き込み完了。起動を確かめています…');
		reading = true;
		setState();
		try {
			device = await readBootInfo();
		} finally {
			reading = false;
		}
		setState();
		showVersion();
		const got = device.version;
		if (got === v.version) {
			flashMsg(`✓ v${v.version} で起動しました`, 'ok');
		} else if (got) {
			flashMsg(`書き込みは完了しましたが、起動したのは v${got} でした`, 'warn');
		} else {
			flashMsg('書き込みは完了しました (起動の確認はできませんでした。本体の画面で版を確かめてください)', 'warn');
		}
	} catch (e) {
		flashMsg(`書き込みに失敗しました: ${e.message}`, 'err');
		flog(String(e.stack || e), 'err');
	} finally {
		window.removeEventListener('beforeunload', onLeave);
		busy = false;
		updateButtons();
	}
}

//==================================================================
//	初期化
//==================================================================
$('btnConnect').onclick = connect;
$('btnDisconnect').onclick = disconnect;
$('btnFlash').onclick = flash;
$('selVer').onchange = showVersion;
for (const r of document.querySelectorAll('input[name=panel]')) r.onchange = checkPanel;
if ('serial' in navigator) {
	navigator.serial.addEventListener('disconnect', (e) => {
		if (e.target === port && !busy) {
			port = null;
			device = { version: null, panel: null };
			$('portInfo').textContent = '';
			flashMsg('USB が外れました', 'warn');
			setState();
			updateButtons();
		}
	});
} else {
	$('unsupported').style.display = 'block';
}

setState();
updateButtons();
loadIndex();
