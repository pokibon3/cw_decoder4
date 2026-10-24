//
//	サービスワーカー (書き込みツール)。web/pwa.js が登録する。
//
//	常にネットワークから取り直し (更新はすぐ反映される)、取れないとき
//	(オフライン) だけ保存しておいた分を返す。保存するのはアプリ本体だけで、
//	ファームウェアの配布物 (firmware/、全版で約 35MB) と版の目録はネットワークに任せる。
//	ASSETS を増減したら CACHE の番号を上げること (古い保存分を消すため)。
//
const CACHE = 'cwdec-flasher-1';
const PREFIX = 'cwdec-flasher-';
const ASSETS = [
	'./',
	'index.html',
	'help.html',
	'flasher.js',
	'md5.js',
	'vendor/esptool-js-0.6.1/bundle.js',
	'manifest.webmanifest',
	'../pwa.js',
	'../icons/flasher-192.png',
	'../icons/flasher-512.png',
];
// 同じオリジンの別アプリ・配布物 (このワーカーでは触らない)
const ROOT = new URL('../', self.registration.scope).pathname;
const SKIP = ['firmware/'].map((p) => ROOT + p);

self.addEventListener('install', (e) => {
	e.waitUntil(caches.open(CACHE).then((c) => c.addAll(ASSETS)).then(() => self.skipWaiting()));
});

self.addEventListener('activate', (e) => {
	e.waitUntil(caches.keys()
		.then((keys) => Promise.all(keys.filter((k) => k.startsWith(PREFIX) && k !== CACHE)
			.map((k) => caches.delete(k))))
		.then(() => self.clients.claim()));
});

self.addEventListener('fetch', (e) => {
	const req = e.request;
	if (req.method !== 'GET') return;
	const url = new URL(req.url);
	if (url.origin !== location.origin) return;
	if (SKIP.some((p) => url.pathname.startsWith(p))) return;
	e.respondWith(fetch(req).then((res) => {
		if (res.ok) {
			const copy = res.clone();
			caches.open(CACHE).then((c) => c.put(req, copy));
		}
		return res;
	}).catch(() => caches.match(req, { ignoreSearch: true })
		.then((hit) => hit || Response.error())));
});
