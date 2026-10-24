//
//	3 つの Web アプリ (デコーダ / 書き込みツール / スコープビューア) 共通の
//	インストール (PWA) 処理。
//	  - 同じディレクトリの sw.js をサービスワーカーとして登録する
//	    (アプリごとにスコープが分かれる: ./ / flasher/ / scope/)
//	  - ブラウザがインストール可能と判断したときだけ btn を出し、押されたら
//	    インストールのダイアログを開く。アドレスバーのアイコンからでも入れられる
//
export function setupPwa(btn) {
	if ('serviceWorker' in navigator && location.protocol !== 'file:') {
		navigator.serviceWorker.register('sw.js').catch(() => { /* 開発中のサーバー等 */ });
	}
	let deferred = null;
	window.addEventListener('beforeinstallprompt', (e) => {
		e.preventDefault();
		deferred = e;
		if (btn) btn.hidden = false;
	});
	window.addEventListener('appinstalled', () => {
		deferred = null;
		if (btn) btn.hidden = true;
	});
	btn?.addEventListener('click', async (e) => {
		e.preventDefault();
		if (!deferred) return;
		deferred.prompt();
		await deferred.userChoice;
		deferred = null;
		btn.hidden = true;
	});
}
