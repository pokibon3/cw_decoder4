//
//	3 つの Web アプリ (デコーダ / 書き込みツール / スコープビューア) 共通の
//	インストール (PWA) 処理。
//	  - 同じディレクトリの sw.js をサービスワーカーとして登録する
//	    (アプリごとにスコープが分かれる: ./ / flasher/ / scope/)
//	  - 「インストール」ボタンは常に出す (インストール済みのアプリとして開いて
//	    いるときだけ隠す)。ブラウザがすぐインストールできる状態ならその場で
//	    ダイアログを開き、そうでなければ (Safari や、ブラウザの準備前) 手順の
//	    説明 helpUrl を開く。ボタンが出たり出なかったりすると、どこから
//	    入れるのか分からなくなるため
//
export function setupPwa(btn, helpUrl) {
	if ('serviceWorker' in navigator && location.protocol !== 'file:') {
		navigator.serviceWorker.register('sw.js').catch(() => { /* 開発中のサーバー等 */ });
	}
	if (!btn) return;
	const standalone = matchMedia('(display-mode: standalone)').matches || navigator.standalone;
	btn.hidden = !!standalone;
	let deferred = null;
	window.addEventListener('beforeinstallprompt', (e) => {
		e.preventDefault();
		deferred = e;
	});
	window.addEventListener('appinstalled', () => {
		deferred = null;
		btn.hidden = true;
	});
	btn.addEventListener('click', async (e) => {
		e.preventDefault();
		if (deferred) {
			deferred.prompt();
			const { outcome } = await deferred.userChoice;
			deferred = null;
			if (outcome === 'accepted') btn.hidden = true;
		} else {
			window.open(helpUrl, '_blank', 'noopener');
		}
	});
}
