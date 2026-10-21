# Hunspell dictionaries bundled with the Nix package and the dev shell:
# English plus every UI language with a free Hunspell dictionary (Chinese has
# none). nixpkgs' hunspellDicts has no Lithuanian, Arabic, Bengali or Hindi,
# so all of them come from one place: LibreOffice's dictionaries at the commit
# tagged libreoffice-25.2.3.1, the same files the Flatpak manifests pin. (The
# AppImage and Snap bundle Debian 13's hunspell-* packages, whose en_US, de, fr
# and ar come from other upstreams: the word lists differ slightly between
# formats.) Installed under the names Debian uses, with each language's
# licence in share/licenses.
{
  lib,
  stdenvNoCC,
  fetchurl,
}:

let
  rev = "d3660b0040ad0dfe76fa61462ba751743ad05463";
  fetch =
    path: sha256:
    fetchurl {
      url = "https://raw.githubusercontent.com/LibreOffice/dictionaries/${rev}/${path}";
      inherit sha256;
    };
  # [ source path, installed name, sha256 ]
  dictionaries = [
    [ "en/en_US.aff" "en_US.aff" "e746c882dd6f303c2c46e7452804b9201115a6942cfeb15f18f8edf774d2e24e" ]
    [ "en/en_US.dic" "en_US.dic" "f0b1a234bd178bdd01875b2a392a9647f888b8fe879f79c52aae62c2759b3647" ]
    [ "en/en_GB.aff" "en_GB.aff" "0fd6ed120ef28957847d98ba5149b117e27116cf81b5aa36208453f6755a36fd" ]
    [ "en/en_GB.dic" "en_GB.dic" "04e90f34f5263bf26780e9c4a442e9ad16584e227af49ddd1b3b21b01df5b29c" ]
    [ "ar/ar.aff" "ar.aff" "cec30b8621001e49618feb05aec1984c5fcfbf7d2ec309901d5cbf66585217a3" ]
    [ "ar/ar.dic" "ar.dic" "2a3e5367f61c1583734db9d66734f5603e6be5c2d227cf5c5cd7e4ca586e34fe" ]
    [ "bn_BD/bn_BD.aff" "bn_BD.aff" "ba270804dfb5cc585be8f8fe1ab1fa346f64d9c5e70fc43a2d3b7f7a1afe81fc" ]
    [ "bn_BD/bn_BD.dic" "bn_BD.dic" "6206ef5475db73ad5f292f7af091ccdf762be0afcb6c4de67fb510fdc208d09a" ]
    [ "de/de_DE_frami.aff" "de_DE.aff" "646bf3333ac69c23e9d794533ee5241d6f755c359e8fe10a648f87613743d594" ]
    [ "de/de_DE_frami.dic" "de_DE.dic" "4ca3c958b0e5545910999bc246f668840bf8ede3df8e5e6790d05edd5a586c38" ]
    [ "es/es_ES.aff" "es_ES.aff" "e73a9bf8e1383f4986a5dc9e2fbed49371c0c61f511c626d15586bd433c1cad9" ]
    [ "es/es_ES.dic" "es_ES.dic" "6975dddec3d5d2c676069537bc67b4b5f786c65c5d4cf6703a82acf779ac9ec1" ]
    [ "fr_FR/fr.aff" "fr.aff" "4730775ff4488c4a9aa4833393c8b2187126336eed766866300a58a75fe874f0" ]
    [ "fr_FR/fr.dic" "fr.dic" "b99447ab2e655c1e4b64509cfd2e6c24821242bff1f971b49eac723e388a2d19" ]
    [ "hi_IN/hi_IN.aff" "hi_IN.aff" "15afdbd926b16bc52344a6f94d965c84f5697839408ce68a5196f19ee606caf9" ]
    [ "hi_IN/hi_IN.dic" "hi_IN.dic" "15459d1fdf566953d2e0bc1374114b76ae41fe8230df6a033aa0da9432d6952b" ]
    [ "id/id_ID.aff" "id_ID.aff" "c625d5b237a489c452cf1f9c666600103e8093667dff89e7030a24217995dc79" ]
    [ "id/id_ID.dic" "id_ID.dic" "775ff17a52801f3d2ee80120952fb44cc9bac6c3cf61740de775e439851a5803" ]
    [ "lt_LT/lt.aff" "lt_LT.aff" "01ec7b4edc369f696c39918aa9b33e5e738005983f3a76ce4817af2ed361b060" ]
    [ "lt_LT/lt.dic" "lt_LT.dic" "1714f38f7b80f35799ab6abb8ff4a40fa1de805e1adc1d4d30c0b5a179887469" ]
    [ "pt_PT/pt_PT.aff" "pt_PT.aff" "975a209fcc892cb382fa5f34a28c391a39668661ce373ae071287809c5fcae24" ]
    [ "pt_PT/pt_PT.dic" "pt_PT.dic" "9d90cfd9fb15312db71fbe46c11f871df67684dae7c218ab270142e7ae68c377" ]
    [ "pt_BR/pt_BR.aff" "pt_BR.aff" "21d8ad2a769a60e17e2b5ea4ef11d4d593a58b9e2a82d642ef82d6a4c5523865" ]
    [ "pt_BR/pt_BR.dic" "pt_BR.dic" "a38bfb26b68ece2834e79fe83e48d5792652970ace12db89d1b9674bf9933183" ]
    [ "ru_RU/ru_RU.aff" "ru_RU.aff" "38ce7d4af78e211e9bafe4bf7e3d6a2c420591136cb738ec6648f8fdf6524cd7" ]
    [ "ru_RU/ru_RU.dic" "ru_RU.dic" "f6047416a0204adbecf3a451b874ec8a97ee37e2cbc714466ef04d8dbcc0d6fc" ]
  ];
  licences = [
    [ "en/license.txt" "df84246204e3c54cd24aebbc23714235f5fc41d754d53b65444a2afbf070130f" ]
    [ "en/README_en_US.txt" "168b4c01cb841f766a72f310b065c04d09cff46376c37a81d799593ce751c371" ]
    [ "en/README_en_GB.txt" "18b5833de60a52ffcb6e5b6b5048bb5a5d910acfecd5b096985b77058223880c" ]
    [ "ar/COPYING.txt" "73480d918cd78d655508e80aa88c022cbd53634524a0f714b5f2519f2cf19124" ]
    [ "bn_BD/COPYING" "204d8eff92f95aac4df6c8122bc1505f468f3a901e5a4cc08940e0ede1938994" ]
    [ "de/COPYING_GPLv2" "e7431ac20e815b6797369e802f9e4bb1b0082485060d8b096235f035d741bd89" ]
    [ "de/COPYING_GPLv3" "8ceb4b9ee5adedde47b31e975c1d90c73ad27b6b165a1dcd80c7c545eb65b903" ]
    [ "de/README_de_DE_frami.txt" "c141f4f79c428b7348b5012836f4ad3db4d124f288f15effc22696dc876512ae" ]
    [ "es/LICENSE.md" "2b70fd5cb8d3b34cc6ae322c969557a2270b99017594addb2531ef5ffd9e1b17" ]
    [ "fr_FR/README_fr.txt" "41af48742037f80ece5338a2a1bbb01111610339af4686ab93f468045659ba6b" ]
    [ "hi_IN/COPYING" "204d8eff92f95aac4df6c8122bc1505f468f3a901e5a4cc08940e0ede1938994" ]
    [ "id/LICENSE-dict" "97628afebc60f026f5c2b25d7491c46a5c4ee61f693e7cfa07fbd2c03605979b" ]
    [ "lt_LT/COPYING" "97c7647bb681f70233ba9698daf7d7837764796cb98ca88abfed38ca03af47a5" ]
    [ "pt_BR/README_pt_BR.txt" "9974ce691fdc1fe731717d7a2dc668244405fdc2bf9bf3367eb9b29e85177c88" ]
    [ "pt_PT/LICENSES.txt" "d2c1cfe2e2dd81c651aec3fda5d1b4b4e7679b9e04f8cdc37586e521837384d1" ]
    [ "ru_RU/README_ru_RU.txt" "262af2f6ad70a61e5ee1332ff44fa8ee50edca819cf33207d8ad6ba6a0c9be52" ]
  ];
in
stdenvNoCC.mkDerivation {
  pname = "lightning-spell-dictionaries";
  version = "25.2.3.1";
  dontUnpack = true;
  installPhase = ''
    runHook preInstall
    install -d $out/share/hunspell
    ${lib.concatMapStrings (d: ''
      install -m644 ${fetch (builtins.elemAt d 0) (builtins.elemAt d 2)} $out/share/hunspell/${builtins.elemAt d 1}
    '') dictionaries}
    ${lib.concatMapStrings (l: ''
      install -Dm644 ${fetch (builtins.elemAt l 0) (builtins.elemAt l 1)} $out/share/licenses/hunspell-dictionaries/${builtins.elemAt l 0}
    '') licences}
    runHook postInstall
  '';
  meta = {
    description = "Hunspell dictionaries for Lightning's spell checker";
    # Per language: GPL, LGPL, MPL and BSD-style terms; see share/licenses.
    license = with lib.licenses; [
      gpl2Plus
      lgpl21Plus
      mpl11
      mpl20
      bsd3
    ];
    platforms = lib.platforms.all;
  };
}
