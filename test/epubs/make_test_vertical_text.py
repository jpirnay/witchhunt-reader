#!/usr/bin/env python3
"""Build test_vertical_text.epub, a Japanese book set vertically, and test_vertical_text_zh_hant.epub, its Traditional
Chinese sibling, for the vertical goldens and the vertical rule checker (test/epub_pipeline/VerticalRulesTest.cpp,
VerticalLayoutTest.cpp).

Language ja, spine page-progression-direction="rtl" (the golden's BOOK line ends ppd=rtl).

Spine 1, as a calibre conversion writes it: the mode only as -webkit-writing-mode, through a class on <body>
(`body.v`), `p { margin: 0; text-indent: 1em }`. It pins, in order:
  - a heading, 第一章　夜の森;
  - ruby (<ruby>冥夜<rt>めいや</rt></ruby>), small kana (ッ, ゃ, ァ), 、。 and the brackets 「」『』（）, ー, ……, ——, ！？;
  - sideways runs: the Latin word Winterfell, 1998, 第13章, and the brackets and marks above (class R or Tr);
  - × and § (upright in a column), and か followed by U+3099 (a combining mark stays with its base);
  - an underline span, a `text-emphasis: filled sesame` span and a `text-emphasis: filled dot` span. The dot's mark is
    U+2022, UAX #50 class R: it is drawn sideways in a column, and Task 14's device check looks at where it lands
    beside the column (the draw path's review rated it at most a 1-2 px offset);
  - a 2 x 2 table with an image in a cell, a block image (a real 200 x 400 grey PNG, so the device decodes it), a
    `float: left` 80 x 80 image and an <hr/>;
  - on the first page, a link to the colophon, <a href="ch2.xhtml#colophon">奥付</a>: the target of Task 14's link
    tap. The golden records how the build lays it out;
  - enough prose for the rule checker's floors: over 1,200 characters in paragraphs of 200 or more, so that at least
    20 of the spine's columns are broken by the layout (47 characters of a 760 px column), and at least 10 sideways
    runs;
  - last, so the pages before it keep their layout, a short paragraph of the small kana ぁぃぅぇぉっゃゅょゎ and
    ァィゥェォッャュョ, which a column places up and right, a 、 ending its first column, and 。 and 、 before 」』）,
    half an em wide there (the rule checker's placed-ink check).
Spine 2, the colophon: <body style="writing-mode: horizontal-tb">, so it stays horizontal and still turns right to
left (the book's progression), an <h2 id="colophon"> and two short paragraphs.

test_vertical_text_zh_hant.epub: language zh-Hant, `html { writing-mode: vertical-rl }` (the standard property, on
<html>: it reaches <body> through the book's CSS), spine page-progression-direction="rtl", one spine. A heading and
about 30 columns of prose with 、。，：？ and 「」: Traditional Chinese centres 、。， in their cell down a column, and its
table keeps every mark full width but on demand (push-in). The last paragraph's first column ends 寫著Taipei。, 1 px
too long for the column (42 characters, a 4 px CJK-Latin gap, the 45 px sideways Taipei and 。 against 736 px): the
DP pushes it in rather than leave 64 px loose before Taipei, and 。 gives its 4 px, so the golden holds a narrowed
centred mark, drawn back by half of what it lost.

Regenerate both with:  python -I test/epubs/make_test_vertical_text.py
then refresh goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import os
import struct
import zipfile
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

CONTAINER = (
    '<?xml version="1.0"?>\n'
    '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
    '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
    'media-type="application/oebps-package+xml"/></rootfiles>\n'
    "</container>\n"
)

CSS = """body { margin: 0; padding: 0 }
body.v { -webkit-writing-mode: vertical-rl }
p { margin: 0; text-indent: 1em }
h2 { margin: 0; text-indent: 0; font-size: 1em }
td { padding: 0 }
.ul { text-decoration: underline }
"""


def png(width, height):
    """A grey diagonal-stripe PNG, 8-bit greyscale, written with zlib."""
    rows = b"".join(b"\x00" + bytes(((x + y) // 8 % 2) * 160 + 40 for x in range(width)) for y in range(height))

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    header = struct.pack(">IIBBBBB", width, height, 8, 0, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")


SPINE1 = (
    "<h2>第一章　夜の森</h2>\n"
    '<p>吾輩は猫である。名前はまだ無い。どこで生れたかとんと見当がつかぬ。何でも薄暗いじめじめした所で'
    "ニャーニャー泣いていた事だけは記憶している。吾輩はここで始めて人間というものを見た。しかもあとで聞くと"
    "それは書生という人間中で一番獰悪な種族であったそうだ。この書生というのは時々我々を捕えて煮て食うという話"
    "である。</p>\n"
    "<p>その夜、<ruby>冥夜<rt>めいや</rt></ruby>の森は深い霧に包まれていた。「誰か、そこにいるのか。」と男は"
    "低く言った。返事はない。ただ風が枯れ葉をさらい、遠くでふくろうがホーホーと鳴くばかりである。男は外套の襟を"
    "立て、足を速めた。詳しくは"
    '<a href="ch2.xhtml#colophon">奥付</a>'
    "を見よ。やがて道は二つに分かれ、一方は丘へ、一方は谷へと続いていた。月は雲に隠れ、あたりは"
    "いっそう暗くなった。</p>\n"
    "<p>老人はこう語った。『北の城Winterfellには、1998年の冬、一人の旅人が訪れたという。その旅人は第13章の"
    "写本を携え、「ここに真実がある」と言ったそうだ。』聞いていた子どもたちは息をのみ、ッと声を詰まらせた。"
    "小さなゃの字、ァの字までが、火に照らされて揺れて見えた。（これは古い言い伝えである。）</p>\n"
    "<p>「そんなはずはない！」「本当に？」――若者は叫んだが、老人は笑って首を振るだけだった。長い長い話が"
    "続いた。ー、……、——と、句読点のように間が置かれ、そのたびに炉の火がぱちりと爆ぜた。2×3は6、§12の"
    "条文にもそう書いてある、と老人は付け加えた。がと書いてガと読ませる古い綴りも、この森の民は好んだ。</p>\n"
    '<p>この<span class="ul">下線を引いた言葉</span>と、<span style="text-emphasis: filled sesame">傍点を打った'
    "言葉</span>と、<span style=\"text-emphasis: filled dot\">黒丸を打った言葉</span>とが、一つの段落に並んで"
    "いる。読む者はゆっくりと目を移し、一行ずつ丁寧に文字を追っていった。雨が屋根を打つ音だけが聞こえていた。</p>\n"
    "<p>夜が明けるころ、森の木々はようやく輪郭を"
    "あらわし、鳥たちが一羽、また一羽と目を覚ました。旅人は荷を背負い直し、霧の晴れていく道をひとり歩き出した。"
    "背後には、昨夜の火の跡が細い煙を上げていた。</p>\n"
    "<p>峠の茶屋では、一人の女が湯を沸かして待っていた。「お疲れでしょう。」と女は言い、欠けた茶碗に熱い茶を"
    "注いだ。旅人は礼を述べて腰を下ろし、しばらく黙って湯気の行方を眺めていた。窓の外には、段々に連なる畑と、"
    "その向こうに霞む山並みが見えた。女は問われるままに、この土地の古い祭りや、冬の長さや、雪の降り積もる夜の"
    "静けさについて語った。旅人はそのどれにも深くうなずき、茶をゆっくりと飲み干した。</p>\n"
    "<p>日が傾くころ、旅人は再び歩き出した。峠を越えると、遠くに町の灯が小さく瞬いているのが見えた。足もとの"
    "道は夕露に濡れ、草の匂いが濃く立ちのぼっていた。彼は立ち止まって深く息を吸い、これまでの長い道のりを"
    "思い返した。数え切れないほどの朝と夜、見知らぬ人々の顔、交わした言葉の一つ一つが、静かに胸の底へ沈んで"
    "いくようだった。やがて最初の家々の屋根が近づき、犬の吠える声が聞こえてきた。</p>\n"
    '<p><img src="images/block.png" alt=""/></p>\n'
    '<p><img src="images/float.png" alt="" style="float: left"/>図の左には小さな挿絵が浮かび、本文はその脇を'
    "回って流れていく。挿絵のあとにも文章は続き、一つの章がゆっくりと終わりへ向かっていった。</p>\n"
    "<hr/>\n"
    "<table>"
    "<tr><td>一行一列</td><td>一行二列</td></tr>"
    '<tr><td><img src="images/cell.png" alt=""/></td><td>二行二列</td></tr>'
    "</table>\n"
    "<p>夜の森の物語は、ここでひとまず幕を閉じる。次の章では、旅人が丘の上の古い塔にたどり着き、そこで長い間"
    "忘れられていた一つの約束を思い出すことになる。それはまた別の夜の話である。</p>\n"
    "<p>小さな仮名を並べておく。ぁぃぅぇぉっゃゅょゎ、ァィゥェォッャュョ。彼は「もう行く。」と言い、娘は"
    "『待って、』と呼びとめた（古い本にはそう書いてある、）。</p>\n"
)

SPINE2 = (
    '<h2 id="colophon">奥付</h2>\n'
    "<p>夜の森　試験用の縦書き見本。</p>\n"
    "<p>この本は組版の試験のために作られた。</p>\n"
)

ZH_HANT_CSS = """html { writing-mode: vertical-rl }
body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 1em }
h2 { margin: 0; text-indent: 0; font-size: 1em }
"""

ZH_HANT = (
    "<h2>第一章　山中夜行</h2>\n"
    "<p>那一夜，山中起了大霧，四周一片寂靜。旅人背著行囊，沿著溪邊的小路慢慢前行，腳下的石子被露水打濕，每走一步"
    "都要格外小心。遠處偶爾傳來幾聲鳥叫，又很快消失在霧裡。他停下腳步，抬頭望了望天空，只見雲層厚重，月亮早已"
    "不知去向，連一顆星星也看不見。</p>\n"
    "<p>「有人嗎？」他低聲問道。沒有人回答，只有風吹過竹林的聲音，沙沙作響。他想起出發之前，村口的老人曾經對他"
    "說：「山裡的路，白天好走，夜裡難行，千萬不要貪快。」於是他放慢腳步，一邊走，一邊留意兩旁的樹影、石頭和"
    "水聲，生怕一不小心就走錯了方向。</p>\n"
    "<p>走了約莫一個時辰，前方忽然出現一點燈光。旅人心中一喜，加快腳步走近，原來是一間小小的茶棚。棚下坐著一位"
    "老婦人，正在爐邊燒水。她見有客人來，便起身招呼：「客人辛苦了，先喝杯熱茶，暖暖身子吧。」旅人連聲道謝，在"
    "木凳上坐下，雙手捧著茶碗，看著熱氣一縷一縷地升起來。</p>\n"
    "<p>老婦人問他從哪裡來、要到哪裡去、家中還有什麼人。旅人一一回答，說自己從南方來，要到北邊的城裡去，尋找"
    "一位多年不見的朋友。老婦人聽了，點點頭說：「北邊的城，我年輕的時候也去過一次。那裡的冬天很長，雪下得很深，"
    "街上的人說話都帶著白氣。」說完，她又替他添了一些茶，還拿出兩塊糕餅給他。</p>\n"
    "<p>天快亮的時候，霧漸漸散了，山的輪廓、樹的枝葉、溪水的波光，都一點一點地清楚起來。旅人起身告辭，老婦人送"
    "他到路口，指著東方說：「沿著這條路一直走，過了兩座橋、一片稻田，就能看見大路了。」旅人再三道謝，背起行囊，"
    "踏上了新的旅程。</p>\n"
    "<p>他一路走，一路回想這一夜的經歷：霧中的小路、竹林的風聲、茶棚的燈光、老婦人的笑容。這些事情雖然平凡，"
    "卻讓他覺得十分溫暖。太陽升起來了，照在田野上，也照在他的臉上。他知道，前面的路還很長，但是他已經不再害怕"
    "了。</p>\n"
    "<p>到了中午，他在一棵大樹下休息，從包袱裡拿出乾糧，慢慢地吃著。樹上的蟬叫個不停，田裡的農人正彎著腰插秧，"
    "偶爾直起身來，擦一擦額上的汗。一個小孩牽著牛從他身邊經過，好奇地看了他一眼，問道：「你是從山那邊來的嗎？」"
    "旅人笑著點頭，小孩便高高興興地跑開了。</p>\n"
    "<p>傍晚時分，他終於走到了大路上。路上來往的車馬、挑擔的商販、趕集回家的婦人，絡繹不絕，熱鬧極了。他找了"
    "一家小客棧住下，洗去一身的塵土，坐在窗前，看著街上的燈火一盞一盞地亮起來。他想，再過三天，就能到北邊的城"
    "了，到那時，不知道朋友是否還記得他。</p>\n"
    "<p>夜裡，客棧的掌櫃送來一壺熱水，順便問他：「客官要往哪裡去？」旅人說要到北邊的城裡找朋友。掌櫃想了想，說："
    "「這幾天北邊下了大雪，路不好走，你最好多買一件棉衣、一雙厚鞋，再帶些乾糧。」旅人點頭稱是，心裡暗暗記下，"
    "打算明天一早就到市集上去買。</p>\n"
    "<p>第二天清晨，市集上已經擠滿了人。賣菜的、賣布的、賣鍋碗瓢盆的，一攤挨著一攤，吆喝聲此起彼落。旅人買了棉衣"
    "和厚鞋，又買了一包炒豆，便匆匆出了城門。北風迎面吹來，帶著一點寒意，他把衣領拉高，大步向前走去。遠方的山頂"
    "已經白了，像是戴上了一頂雪帽。</p>\n"
    "<p>第三天傍晚時分，旅人在城門口的小店裡，找到了朋友多年前寄來的一封信，信封上用英文寫著Taipei。信裡說，他已經"
    "搬到南方的城市去了，請旅人不必再來找他。旅人讀完信，在店門口站了很久。</p>\n"
)


def chapter(title, body, body_attrs="", lang="ja"):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE html>\n'
        f'<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{lang}" lang="{lang}">\n'
        f'<head>\n  <title>{title}</title>\n  <link rel="stylesheet" type="text/css" href="style.css"/>\n</head>\n'
        f"<body{body_attrs}>\n{body}</body>\n</html>\n"
    )


OPF = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
    '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
    "    <dc:title>Vertical Text</dc:title>\n"
    "    <dc:creator>Test Corpus</dc:creator>\n"
    "    <dc:language>ja</dc:language>\n"
    '    <dc:identifier id="bookid">vertical-text</dc:identifier>\n'
    "  </metadata>\n"
    "  <manifest>\n"
    '    <item id="css" href="style.css" media-type="text/css"/>\n'
    '    <item id="block" href="images/block.png" media-type="image/png"/>\n'
    '    <item id="float" href="images/float.png" media-type="image/png"/>\n'
    '    <item id="cell" href="images/cell.png" media-type="image/png"/>\n'
    '    <item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>\n'
    '    <item id="ch2" href="ch2.xhtml" media-type="application/xhtml+xml"/>\n'
    "  </manifest>\n"
    '  <spine page-progression-direction="rtl">\n'
    '    <itemref idref="ch1"/>\n'
    '    <itemref idref="ch2"/>\n'
    "  </spine>\n"
    "</package>\n"
)

ZH_HANT_OPF = (
    '<?xml version="1.0" encoding="UTF-8"?>\n'
    '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
    '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
    "    <dc:title>Vertical Text (Traditional Chinese)</dc:title>\n"
    "    <dc:creator>Test Corpus</dc:creator>\n"
    "    <dc:language>zh-Hant</dc:language>\n"
    '    <dc:identifier id="bookid">vertical-text-zh-hant</dc:identifier>\n'
    "  </metadata>\n"
    "  <manifest>\n"
    '    <item id="css" href="style.css" media-type="text/css"/>\n'
    '    <item id="ch1" href="ch1.xhtml" media-type="application/xhtml+xml"/>\n'
    "  </manifest>\n"
    '  <spine page-progression-direction="rtl">\n'
    '    <itemref idref="ch1"/>\n'
    "  </spine>\n"
    "</package>\n"
)


def write_book(name, files):
    out = os.path.join(HERE, name)
    with zipfile.ZipFile(out, "w") as z:
        for path, data, method in files:
            # A fixed date keeps a regenerated book byte-identical.
            info = zipfile.ZipInfo(path, date_time=(2026, 10, 29, 0, 0, 0))
            info.compress_type = method
            z.writestr(info, data)
    print("wrote", out)


def main():
    stored, deflated = zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED
    write_book(
        "test_vertical_text.epub",
        [
            ("mimetype", "application/epub+zip", stored),
            ("META-INF/container.xml", CONTAINER, deflated),
            ("OEBPS/content.opf", OPF, deflated),
            ("OEBPS/style.css", CSS, deflated),
            ("OEBPS/ch1.xhtml", chapter("Vertical", SPINE1, ' class="v"'), deflated),
            ("OEBPS/ch2.xhtml", chapter("Colophon", SPINE2, ' style="writing-mode: horizontal-tb"'), deflated),
            ("OEBPS/images/block.png", png(200, 400), stored),
            ("OEBPS/images/float.png", png(80, 80), stored),
            ("OEBPS/images/cell.png", png(60, 60), stored),
        ],
    )
    write_book(
        "test_vertical_text_zh_hant.epub",
        [
            ("mimetype", "application/epub+zip", stored),
            ("META-INF/container.xml", CONTAINER, deflated),
            ("OEBPS/content.opf", ZH_HANT_OPF, deflated),
            ("OEBPS/style.css", ZH_HANT_CSS, deflated),
            ("OEBPS/ch1.xhtml", chapter("Vertical", ZH_HANT, lang="zh-Hant"), deflated),
        ],
    )


if __name__ == "__main__":
    main()
