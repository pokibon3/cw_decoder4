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
//	接続中はシリアルモニターを開いておき、起動ログの [boot] / [lcd] 行から
//	いまの版とパネルを読む。書き込みの間だけモニターを閉じて esptool-js に渡す。
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
const MON_BAUD = 115200;
// スコープログの行 (src/scopelog.cpp): 英大文字 1 字 + 空白で始まる
const SCOPE_LINE = /^[HTSWCELX] /;
const MON_KEEP = 20000;          // 保存用に持つ行数
const MON_SHOW = 1500;           // 画面に出す行数

let index = null;                // versions.json
let port = null;
let busy = false;                // 書き込み中
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
	const v = device.version ? `v${device.version}` : '<span class="dim">不明</span>';
	const p = device.panel || '<span class="dim">不明</span>';
	el.innerHTML = `本体のファーム: <b>${v}</b>&nbsp;&nbsp;パネル: <b>${p}</b>` +
		(device.version ? '' : ' <span class="dim">(起動ログを読み取り中。出ないときは「本体をリセット」)</span>');
}

function updateButtons() {
	const connected = !!port;
	$('btnConnect').disabled = connected || busy || !('serial' in navigator);
	$('btnDisconnect').disabled = !connected || busy;
	$('btnFlash').disabled = !connected || busy || !index;
	$('btnMon').disabled = !connected || busy;
	$('btnMon').textContent = monitor.running ? 'モニター停止' : 'モニター開始';
	$('btnReset').disabled = !connected || busy || !monitor.running;
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
	$('flashMsg').className = cls;
	$('flashMsg').textContent = s;
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
//	シリアルモニター
//==================================================================
const monitor = {
	running: false,
	reader: null,
	lines: [],                   // 全行 (保存用、MON_KEEP まで)
	partial: '',
	dirty: false,
	closed: null,                // 読み取りループの終了 Promise
};

function sleep(ms) { return new Promise((r) => setTimeout(r, ms)); }

async function monitorStart() {
	if (monitor.running || !port) return;
	await port.open({ baudRate: MON_BAUD });
	// DTR / RTS は基板の自動リセット回路につながっている (RTS = EN、DTR = IO0)。
	// 開いた直後に両方とも下げて、通常起動のまま走らせる
	await port.setSignals({ dataTerminalReady: false, requestToSend: false });
	monitor.running = true;
	monitor.reader = port.readable.getReader();
	const dec = new TextDecoder('utf-8');
	monitor.closed = (async () => {
		try {
			for (;;) {
				const { value, done } = await monitor.reader.read();
				if (done) break;
				monitorFeed(dec.decode(value, { stream: true }));
			}
		} catch (e) {
			if (monitor.running) monitorFeed(`\n[モニター: ${e.message}]\n`);
		} finally {
			monitor.reader.releaseLock();
		}
	})();
	updateButtons();
}

async function monitorStop() {
	if (!monitor.running) return;
	monitor.running = false;
	try { await monitor.reader.cancel(); } catch { /* 既に切れている */ }
	await monitor.closed;
	try { await port.close(); } catch { /* 抜かれた */ }
	updateButtons();
}

//	本体をリセットして起動ログを出させる (EN を 100ms 落とす、IO0 は High のまま)
async function monitorReset() {
	if (!monitor.running) return;
	await port.setSignals({ dataTerminalReady: false, requestToSend: true });
	await sleep(100);
	await port.setSignals({ requestToSend: false });
}

function monitorFeed(text) {
	const parts = (monitor.partial + text).split(/\r?\n/);
	monitor.partial = parts.pop();
	for (const line of parts) {
		monitor.lines.push(line);
		parseBootLine(line);
	}
	if (monitor.lines.length > MON_KEEP) monitor.lines.splice(0, monitor.lines.length - MON_KEEP);
	monitor.dirty = true;
}

//	起動ログからいまの版とパネルを読む
//	  [lcd] panel = ST7789
//	  [boot] v3.4 build Oct 24 2026 10:27:01  running=app0 ...
function parseBootLine(line) {
	let m = line.match(/^\[boot\] v(\d+(?:\.\d+)+)/);
	if (m) {
		device.version = m[1];
		onDeviceInfo();
	}
	m = line.match(/^\[lcd\] panel = (ST7789|ILI9341)/);
	if (m) {
		device.panel = m[1];
		// 本体のパネルに合わせる。書き込み中 (直後の起動確認を含む) は
		// ユーザーの選択を動かさない
		if (!busy) document.querySelector(`input[name=panel][value=${m[1]}]`).checked = true;
		onDeviceInfo();
	}
}

let bootWaiter = null;           // 書き込み後の起動確認
function onDeviceInfo() {
	setState();
	showVersion();
	if (bootWaiter && device.version) bootWaiter(device.version);
}

function renderMonitor() {
	if (monitor.dirty) {
		monitor.dirty = false;
		const hide = $('hideScope').checked;
		const src = hide ? monitor.lines.filter((l) => !SCOPE_LINE.test(l)) : monitor.lines;
		const pre = $('mon');
		pre.textContent = src.slice(-MON_SHOW).join('\n');
		if ($('autoScroll').checked) pre.scrollTop = pre.scrollHeight;
	}
	requestAnimationFrame(renderMonitor);
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
		await monitorStart();
		await monitorReset();    // 起動ログから版とパネルを読む
	} catch (e) {
		flashMsg(`ポートを開けません: ${e.message} — ほかのソフトが使っていないか確認してください`, 'err');
		port = null;
		setState();
	}
	updateButtons();
}

async function disconnect() {
	await monitorStop();
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

		await monitorStop();
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
		device = { version: null, panel: null };
		setState();
		const booted = new Promise((res) => {
			bootWaiter = res;
			setTimeout(() => res(null), 10000);
		});
		await monitorStart();
		await monitorReset();
		const got = await booted;
		bootWaiter = null;
		if (got === v.version) {
			flashMsg(`✓ v${v.version} で起動しました`, 'ok');
		} else if (got) {
			flashMsg(`書き込みは完了しましたが、起動したのは v${got} でした`, 'warn');
		} else {
			flashMsg('書き込みは完了しました (起動ログを確認できませんでした。モニターで確認してください)', 'warn');
		}
	} catch (e) {
		flashMsg(`書き込みに失敗しました: ${e.message}`, 'err');
		flog(String(e.stack || e), 'err');
		// モニターを戻しておく (失敗後も状態を見られるように)
		try { if (port && !monitor.running) await monitorStart(); } catch { /* 抜かれた */ }
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
$('btnMon').onclick = async () => {
	try {
		if (monitor.running) await monitorStop();
		else await monitorStart();
	} catch (e) {
		flashMsg(`モニターを開けません: ${e.message}`, 'err');
	}
	updateButtons();
};
$('btnReset').onclick = () => monitorReset();
$('btnClear').onclick = () => { monitor.lines = []; monitor.dirty = true; };
$('btnSave').onclick = () => {
	const blob = new Blob([monitor.lines.join('\n') + '\n'], { type: 'text/plain' });
	const a = document.createElement('a');
	a.href = URL.createObjectURL(blob);
	a.download = `cwdecoder-log-${new Date().toISOString().replace(/[:.]/g, '-')}.txt`;
	a.click();
	URL.revokeObjectURL(a.href);
};
$('hideScope').onchange = () => { monitor.dirty = true; };

if ('serial' in navigator) {
	navigator.serial.addEventListener('disconnect', (e) => {
		if (e.target === port && !busy) {
			monitor.running = false;
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
requestAnimationFrame(renderMonitor);
loadIndex();
