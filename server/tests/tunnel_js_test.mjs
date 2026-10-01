// The web client's tunnel crypto (the block between the markers in server/web/index.html) against the RFC and
// python-derived vectors that server/tests/relay_test.cpp checks libsodium with. Run: node tunnel_js_test.mjs index.html
import { readFileSync } from 'node:fs';

const html = readFileSync(process.argv[2], 'utf8');
const begin = html.indexOf('// --- tunnel crypto begin ---'), end = html.indexOf('// --- tunnel crypto end ---');
if (begin < 0 || end < 0) { console.log('FAIL  no tunnel crypto block in ' + process.argv[2]); process.exit(1); }
const Tunnel = new Function(html.slice(begin, end) + '\nreturn Tunnel;')();

let failures = 0;
const expect = (ok, what) => { console.log((ok ? '  ok    ' : '  FAIL  ') + what); if (!ok) failures++; };
const hex = (b) => Array.from(b, (x) => x.toString(16).padStart(2, '0')).join('');
const unhex = (s) => Uint8Array.from(s.match(/../g), (x) => parseInt(x, 16));
const te = new TextEncoder();

console.log('chacha20 and poly1305 (RFC 8439)');
{
  const key = unhex('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f');
  const ct = Tunnel.chacha20(key, 1, unhex('000000000000004a00000000'), te.encode("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it."));
  expect(hex(ct).startsWith('6e2e359a2568f98041ba0728dd0d6981e97e7aec1d4360c20a27afccfd9fae0bf91b65c5524733ab'), '2.4.2: the ChaCha20 ciphertext');
  const tag = Tunnel.poly1305(unhex('85d6be7857556d337f4452fe42d506a80103808afb0db2fd4abff6af4149f51b'), te.encode('Cryptographic Forum Research Group'));
  expect(hex(tag) === 'a8061dc1305136c6c22b8baf0c0127a9', '2.5.2: the Poly1305 tag');
  const aeadKey = unhex('808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f');
  const nonce = unhex('070000004041424344454647'), ad = unhex('50515253c0c1c2c3c4c5c6c7');
  const plain = te.encode("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
  const sealed = Tunnel.aeadSeal(aeadKey, nonce, plain, ad);
  expect(hex(sealed).startsWith('d31a8d34648e60db7b86afbc53ef7ec2a4aded51296e08fea9e2b5a736ee62d6') && hex(sealed).endsWith('1ae10b594f09e26a7e902ecbd0600691'), '2.8.2: the AEAD ciphertext and tag');
  const opened = Tunnel.aeadOpen(aeadKey, nonce, sealed, ad);
  expect(opened && hex(opened) === hex(plain), 'it opens again');
  sealed[3] ^= 1;
  expect(Tunnel.aeadOpen(aeadKey, nonce, sealed, ad) === null, 'one flipped byte fails');
}

console.log('xchacha20-poly1305');
{
  expect(hex(Tunnel.hchacha20(unhex('000102030405060708090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f'), unhex('000000090000004a0000000031415927'))) === '82413b4227b27bfed30e42508a877d73a0f9e4d58a74a853c12ec41326d3ecdc',
         'HChaCha20 (draft-irtf-cfrg-xchacha 2.2.1)');
  const key = unhex('808182838485868788898a8b8c8d8e8f909192939495969798999a9b9c9d9e9f');
  const nonce = unhex('404142434445464748494a4b4c4d4e4f5051525354555657'), ad = unhex('50515253c0c1c2c3c4c5c6c7');
  const plain = te.encode("Ladies and Gentlemen of the class of '99: If I could offer you only one tip for the future, sunscreen would be it.");
  const sealed = Tunnel.xSeal(key, nonce, plain, ad);
  expect(hex(sealed) === 'bd6d179d3e83d43b9576579493c0e939572a1700252bfaccbed2902c21396cbb731c7f1b0b4aa6440bf3a82f4eda7e39ae64c6708c54c216cb96b72e1213b4522f8c9ba40db5d945b11b69b982c1bb9e3f3fac2bc369488f76b2383565d3fff921f9664c97637da9768812f615c68b13b52ec0875924c1c7987947deafd8780acf49',
         'the libsodium test vector');
  expect(hex(Tunnel.xOpen(key, nonce, sealed, ad)) === hex(plain), 'it opens again');
}

console.log('x25519 and hkdf (WebCrypto)');
{
  const jwk = (sk, pk) => ({ kty: 'OKP', crv: 'X25519', d: Tunnel.b64url(unhex(sk)), x: Tunnel.b64url(unhex(pk)) });
  const shared = await Tunnel.dh(jwk('77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a', '8520f0098930a754748b7ddcb43ef75a0dbf3a0d26381af4eba4a98eaa9b4e6a'),
                                 unhex('de9edb7d7b7dc1b4d35b61c2ece435373f8343c85b78674dadfc7e146f882b4f'));
  expect(hex(shared) === '4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742', 'RFC 7748 6.1');
  const okm = await Tunnel.hkdf(unhex('000102030405060708090a0b0c'), new Uint8Array(22).fill(0x0b), unhex('f0f1f2f3f4f5f6f7f8f9'), 42);
  expect(hex(okm) === '3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865', 'RFC 5869 test case 1');
}

console.log('the session: the same keys and frames as the C++ side');
{
  const v = {
    phone_static_sk: '8bf52fdf679746bed5c360faabade18348c82763802bf96e6e0c4559470b1ec7', phone_static_pk: 'b924d2559310192ebe65016cfeec9a2aaeab67def816183ca3beeeea6c0c754b',
    phone_eph_sk: '0972a69e84b538b4d37aeecd9e9cb23d381cebd428a017baa75bca4b372122b7', phone_eph_pk: 'be68742ca3ca80acd3814309b25c7c41b9b58b96d87a66bcd8454d9e8b8da458',
    home_static_pk: '22d18dedc370ccc8a0ddd45e6da7b406fa7287fd073c85a009e5caf192131e32', home_eph_pk: 'b2f66c22a9bf77d235fb089a021c194840fd2008a60f47685251b2cb9630840d',
    to_home: '4e9588819afb1555ae99e31bfb04279d0bff0bea31178470fc72ef93f0805c17', to_phone: '4bb80b6df72d6a086ed7769fee3105cae2e39298f563bb7c44b52211edca1eae',
    plain0: '01000000077b226d6574686f64223a22474554222c2270617468223a222f6170692f737461747573222c2268656164657273223a7b7d7d0a',
    body0: '00000000000000000000000000000000000000000000000089af5f3f097bb2e3dfb40f1a891728bb9bcb3f41e849cff2727a9d7398324b3c1da03816178d50b37403149bd5173e66ba556b1dde27654226d4cd633e2d5d86f229e71358d2cefe',
    body1: '0000000000000001000000000000000000000000000000001239476723a0194ad68846b9cfc3fced83e59588f2',
  };
  const pair = (sk, pk) => ({ privateJwk: { kty: 'OKP', crv: 'X25519', d: Tunnel.b64url(unhex(sk)), x: Tunnel.b64url(unhex(pk)) }, publicRaw: unhex(pk) });
  const keys = await Tunnel.deriveSession(pair(v.phone_static_sk, v.phone_static_pk), pair(v.phone_eph_sk, v.phone_eph_pk), unhex(v.home_static_pk), unhex(v.home_eph_pk));
  expect(hex(keys.toHome) === v.to_home && hex(keys.toPhone) === v.to_phone, 'both directional keys');
  const sealer = new Tunnel.Sealer(keys.toHome);
  const m0 = Tunnel.message(Tunnel.Kind.Request, 7, te.encode('{"method":"GET","path":"/api/status","headers":{}}\n'));
  expect(hex(m0) === v.plain0, 'the request message layout');
  expect(hex(sealer.seal(m0)) === v.body0, 'frame 0 seals to the vector');
  expect(hex(sealer.seal(Tunnel.message(Tunnel.Kind.End, 7, new Uint8Array(0)))) === v.body1, 'frame 1 carries counter 1');
  const opener = new Tunnel.Opener(keys.toHome);
  expect(opener.open(unhex(v.body1)) === null, 'a frame ahead of the counter is refused');
  expect(hex(opener.open(unhex(v.body0))) === v.plain0, 'the next frame opens');
  expect(opener.open(unhex(v.body0)) === null, 'the same frame again (a replay) is refused');
  expect(opener.open(unhex(v.body1)) !== null, 'then the one after');
  const h = Tunnel.parseHello(Tunnel.hello(unhex(v.phone_static_pk), unhex(v.phone_eph_pk)));
  expect(h && hex(h.statik) === v.phone_static_pk && hex(h.eph) === v.phone_eph_pk, 'the hello round trip');
  const f = Tunnel.concat(Tunnel.frame(unhex(v.body0)), Tunnel.frame(new Uint8Array(0)));
  const [bodies, rest] = Tunnel.splitFrames(Tunnel.concat(f, Tunnel.frame(unhex(v.body1)).subarray(0, 10)));
  expect(bodies.length === 2 && bodies[1].length === 0 && rest.length === 10, 'frames split at their lengths, a keepalive and a partial frame included');
}

console.log(failures ? failures + ' FAILED' : 'all passed');
process.exit(failures ? 1 : 0);
