// Tells app.js whether MAIC serves the page; over file:// it also loads the answers kept beside the page.
window.REVIEW_HTTP = /^https?:$/.test(location.protocol);
if (!window.REVIEW_HTTP) document.write('<script src="./answers.js"><\/script>');
