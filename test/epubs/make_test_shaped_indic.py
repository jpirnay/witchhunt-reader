#!/usr/bin/env python3
"""Build the four shaped-Indic books: test_shaped_devanagari.epub (hi), test_shaped_bengali.epub (bn),
test_shaped_tamil.epub (ta) and test_shaped_malayalam.epub (ml). ShapingRulesTest checks its rules on every page of
each, laid out with its font (test/fixtures/fonts/NotoSans<Script>-Book-Regular.ttf, made from the books by
lib/EpdFont/scripts/make_shaped_book_fonts.py), and each has a golden shaped with that font; the Devanagari book also
has an _unshaped golden, laid out with no face (IndicReorder's forms).

Each book holds two chapters of the Universal Declaration of Human Rights in its language, the UN's translation as the
UDHR in XML project keeps it, at eric-muller/udhr commit 588b3f4b2d0467aff54842a4b926551b69d5a66a:
  - Hindi:     https://raw.githubusercontent.com/eric-muller/udhr/588b3f4b2d0467aff54842a4b926551b69d5a66a/data/udhr/udhr_hin.xml
  - Bengali:   https://raw.githubusercontent.com/eric-muller/udhr/588b3f4b2d0467aff54842a4b926551b69d5a66a/data/udhr/udhr_ben.xml
  - Tamil:     https://raw.githubusercontent.com/eric-muller/udhr/588b3f4b2d0467aff54842a4b926551b69d5a66a/data/udhr/udhr_tam.xml
  - Malayalam: https://raw.githubusercontent.com/eric-muller/udhr/588b3f4b2d0467aff54842a4b926551b69d5a66a/data/udhr/udhr_mal.xml
  - English (the Latin sentence and the --long English book):
               https://raw.githubusercontent.com/eric-muller/udhr/588b3f4b2d0467aff54842a4b926551b69d5a66a/data/udhr/udhr_eng.xml
The texts are the files' paragraphs with their whitespace collapsed, in NFC (the Hindi and Bengali files spell a few
nukta letters precomposed, U+0958, U+095B, U+095D and U+09DF, which NFC writes as the letter and its nukta). This
script holds them and never fetches. Terms: OHCHR's UDHR Translation Project lets its translations be reproduced with a
link to it as the source ("Permissions", https://www.ohchr.org/en/human-rights/universal-declaration/universal-declaration-human-rights/about-universal-declaration-human-rights-translation-project),
and the UDHR in XML project, which carries them, calls them "part of our commons" (website/index.xml at that commit).

  - Chapter 1: the declaration's title (<h2>), the preamble's title where the translation has one (<h3>), and the
    preamble's first two paragraphs.
  - Chapter 2: Articles 1-3, each under its <h2> title. Article 1 sets a word <b>, a word <i> and a word in an inline
    font-size: 80% span. Then a paragraph of cases, and a Latin sentence (Article 1 in English, lang="en"), whose
    lines are laid out as any Latin book's:
    - a converter's same-style split of the script's name in brackets ((<span>हि</span><span>न्दी</span>)): one word;
    - a vowel sign in a <b> of its own (क<b>ि</b>): one word, in the first token's style;
    - a bold letter between syllables (क<b>म</b>ल): three tokens, each shaped with its neighbours as context; in
      Bengali also a bold syllable whose e-kar would take its word-start form without that context (যে<b>হে</b>তু);
    - a superscript letter glued to a word (नीति<sup>क</sup>), kept apart the same way;
    - Hindi: a half form with ZWJ (क्\u200dषेत्रीय) and a conjunct pair with ZWNJ (क्\u200cषेत्रीय), joiners the
      parser keeps; Malayalam's chillus with ZWJ at a word's end are the translation's own (ലോകത്തില്\u200d).
  - Malayalam's text holds two words past the 32 code points one shaping call takes (the piecewise loop), and the
    book ends with a 2-column table whose second cell holds a compound wider than the cell, split between syllables.
  - The punctuation, brackets and dandas are the translations' own. Tamil and Malayalam end their sentences with a full
    stop, not a danda, so their books hold none.

With --long <dir>, writes instead the two timing books of the device runs into <dir> (never committed):
test_shaped_long_hi.epub and test_shaped_long_en.epub, each a short first chapter and a second of the declaration's
paragraphs repeated to 2,500 words; the English one's second chapter has the Hindi one's paragraphs, each of the same
word count.

Regenerate with:  python -I test/epubs/make_test_shaped_indic.py  (CPython 3.12; see write())
then the fonts (lib/EpdFont/scripts/make_shaped_book_fonts.py) and the goldens: UPDATE_GOLDENS=1 ctest -R EpubPipeline
"""

import argparse
import os
import unicodedata
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))

CONTAINER = (
    '<?xml version="1.0"?>\n'
    '<container version="1.0" xmlns="urn:oasis:names:tc:opendocument:xmlns:container">\n'
    '  <rootfiles><rootfile full-path="OEBPS/content.opf" '
    'media-type="application/oebps-package+xml"/></rootfiles>\n'
    "</container>\n"
)

CSS = """body { margin: 0; padding: 0 }
p { margin: 0; text-indent: 1em }
"""

HINDI = {
    "title": "मानव अधिकारों की सार्वभौम घोषणा",
    "preamble_title": "प्रस्तावना",
    "preamble": [
        "चूंकि मानव परिवार के सभी सदस्यों के जन्मजात गौरव और समान तथा अविच्छिन्न अधिकार की स्वीकृति ही विश्व-शान्ति, न्याय और स्वतन्त्रता की बुनियाद है,",
        "चूंकि मानव अधिकारों के प्रति उपेक्षा और घृणा के फलस्वरूप ही ऐसे बर्बर कार्य हुए जिनसे मनुष्य की आत्मा पर अत्याचार किया गया, चूंकि एक ऐसी विश्व-व्यवस्था की उस स्थापना को ( जिसमें लोगों को भाषण और धर्म की आज़ादी तथा भय और अभाव से मुक्ति मिलेगी ) सर्वसाधारण के लिए सर्वोच्च आकांक्षा घोषित किया गया है,",
    ],
    "articles": [
        ("अनुच्छेद १.", [
            "सभी मनुष्यों को गौरव और अधिकारों के मामले में जन्मजात स्वतन्त्रता और समानता प्राप्त है । उन्हें बुद्धि और अन्तरात्मा की देन प्राप्त है और परस्पर उन्हें भाईचारे के भाव से बर्ताव करना चाहिए ।",
        ]),
        ("अनुच्छेद २.", [
            "सभी को इस घोषणा में सन्निहित सभी अधिकारों और आज़ादियों को प्राप्त करने का हक़ है और इस मामले में जाति, वर्ण, लिंग, भाषा, धर्म, राजनीति या अन्य विचार-प्रणाली, किसी देश या समाज विशेष में जन्म, सम्पत्ति या किसी प्रकार की अन्य मर्यादा आदि के कारण भेदभाव का विचार न किया जाएगा ।",
            "इसके अतिरिक्त, चाहे कोई देश या प्रदेश स्वतन्त्र हो, संरक्षित हो, या स्त्रशासन रहित हो या परिमित प्रभुसत्ता वाला हो, उस देश या प्रदेश की राजनैतिक, क्षेत्रीय या अन्तर्राष्ट्रीय स्थिति के आधार पर वहां के निवासियों के प्रति कोई फ़रक़ न रखा जाएगा ।",
        ]),
        ("अनुच्छेद ३.", [
            "प्रत्येक व्यक्ति को जीवन, स्वाधीनता और वैयक्तिक सुरक्षा का अधिकार है ।",
        ]),
    ],
    # Article 1's words set <b>, <i> and at 80%.
    "bold": "गौरव",
    "italic": "स्वतन्त्रता",
    "small": "समानता",
    "cases": (
        "(<span>हि</span><span>न्दी</span>), क<b>ि</b>, क<b>म</b>ल, नीति<sup>क</sup>,"
        " क्\u200dषेत्रीय, क्\u200cषेत्रीय ।"
    ),
}

BENGALI = {
    "title": "মানবাধিকারের সার্বজনীন ঘোষণাপত্র",
    "preamble_title": "মুখবন্ধ",
    "preamble": [
        "যেহেতু মানব পরিবারের সকল সদস্যের সমান ও অবিচ্ছেদ্য অধিকারসমূহ এবং সহজাত মর্যাদার স্বীকৃতি\u200cই হচ্ছে বিশ্বে শান্তি, স্বাধীনতা এবং ন্যায়বিচারের ভিত্তি;",
        "যেহেতু মানব অধিকারের প্রতি অবজ্ঞা এবং ঘৃণার ফলে মানুবের বিবেক লাঞ্ছিত বোধ করে এমন সব বর্বরোচিত ঘটনা সংঘটিত হয়েছে এবং যেহেতু এমন একটি পৃথিবীর উদ্ভবকে সাধারণ মানুষের সর্বোচ্চ কাঙ্খা রূপে ঘোষণা করা হয়েছে, যেখানে সকল মানুষ ধর্ম এবং বাক স্বাধীনতা ভোগ করবে এবং অভাব ও শংকামুক্ত জীবন যাপন করবে;",
    ],
    "articles": [
        ("ধারা ১", [
            "সমস্ত মানুষ স্বাধীনভাবে সমান মর্যাদা এবং অধিকার নিয়ে জন্মগ্রহণ করে। তাঁদের বিবেক এবং বুদ্ধি আছে; সুতরাং সকলেরই একে অপরের প্রতি ভ্রাতৃত্বসুলভ মনোভাব নিয়ে আচরণ করা উচিত।",
        ]),
        ("ধারা ২", [
            "এ ঘোষণায় উল্লেখিত স্বাধীনতা এবং অধিকারসমূহে গোত্র, ধর্ম, বর্ণ, শিক্ষা, ভাষা, রাজনৈতিক বা অন্যবিধ মতামত, জাতীয় বা সামাজিক উত্\u200dপত্তি, জন্ম, সম্পত্তি বা অন্য কোন মর্যাদা নির্বিশেষে প্রত্যেকের\u200cই সমান অধিকার থাকবে।",
            "কোন দেশ বা ভূখণ্ডের রাজনৈতিক, সীমানাগত বা আন্তর্জাতিক মর্যাদার ভিত্তিতে তার কোন অধিবাসীর প্রতি কোনরূপ বৈষম্য করা হবেনা; সে দেশ বা ভূখণ্ড স্বাধীন\u200cই হোক, হোক অছিভূক্ত, অস্বায়ত্বশাসিত কিংবা সার্বভৌমত্বের অন্য কোন সীমাবদ্ধতায় বিরাজমান।",
        ]),
        ("ধারা ৩", [
            "জীবন, স্বাধীনতা এবং দৈহিক নিরাপত্তায় প্রত্যেকের অধিকার আছে।",
        ]),
    ],
    "bold": "মর্যাদা",
    "italic": "অধিকার",
    "small": "স্বাধীনভাবে",
    # যে<b>হে</b>তু: the bold syllable's e-kar takes no word-start form, its neighbour being its context.
    "cases": "(<span>বাং</span><span>লা</span>), ক<b>ি</b>, ক<b>ম</b>ল, যে<b>হে</b>তু, নীতি<sup>ক</sup>।",
}

TAMIL = {
    "title": "மனித உரிமைகள் பற்றிய உலகப் பிரகடனம்",
    "preamble_title": "",
    "preamble": [
        "மனிதக் குடும்பத்தினைச் சேர்ந்த யாவரதும் உள்ளார்ந்த மரியாதையையும், அவர்கள் யாவரதும் சமமான, மாற்றத்திற்குட்படுத்த முடியாத உரிமைகளையும் அங்கீகரித்தலே உலகத்தில் சுதந்திரம், நீதி, அமைதி என்பவற்றுக்கு அடிப்படையாகவுள்ளதாதலாலும்,",
        "மனித உரிமைகளை அவமதித்தலும் இகழ்தலும், மனிதகுலத்தின் மனசாட்சியை சீற்றத்திற்குள்ளாக்கியுள்ள காட்டுமிராண்டித்தனமான செயல்களுக்கு இடமளித்துள்ளதாதலாலும், பேச்சுச் சுதந்திரம், நம்பிக்கைச் சுதந்திரம், அச்சத்திலிருந்தும் வறுமையிலிருந்தும் விடுதலை ஆகியனவற்றை மனிதன் முழுமையாக அனுபவிக்கத்தக்க ஒரு உலகின் வருகையே சாதாரண மக்களின் மிகவுயர்ந்த குறிக்கோளாக எடுத்துச் சாற்றப்பட்டுள்ளதாதலாலும்,",
    ],
    "articles": [
        ("உறுப்புரை 1", [
            "மனிதப் பிறிவியினர் சகலரும் சுதந்திரமாகவே பிறக்கின்றனர்; அவர்கள் மதிப்பிலும், உரிமைகளிலும் சமமானவர்கள், அவர்கள் நியாயத்தையும் மனச்சாட்சியையும் இயற்பண்பாகப் பெற்றவர்கள். அவர்கள் ஒருவருடனொருவர் சகோதர உணர்வுப் பாங்கில் நடந்துகொள்ளல் வேண்டும்.",
        ]),
        ("உறுப்புரை 2", [
            "இனம், நிறம், பால், மொழி, மதம், அரசியல் அல்லது வேறு கருத்துடைமை, தேசிய அல்லது சமூகத் தோற்றம், சொத்து, பிறப்பு அல்லது பிற அந்தஸ்து என்பன போன்ற எத்தகைய வேறுபாடுமின்றி, இப்பிரகடனத்தில் தரப்பட்டுள்ள எல்லா உரிமைகளுக்கும் சுதந்திரங்களுக்கும் எல்லோரும் உரித்துடையவராவர்.",
            "மேலும், எவரும் அவரவருக்கு உரித்துள்ள நாட்டின் அல்லது ஆள்புலத்தின் அரசியல், நியாயாதிக்க அல்லது சர்வதேச அங்கிகார நிலைப்பாட்டின் அடிப்படையில் — அது தனியாட்சி நாடாக, நம்பிக்கைப் பொறுப்பு நாடாக, தன்னாட்சியற்ற நாடாக அல்லது இறைமை வேறேதேனும் வகையில் கட்டுப்படுத்தப்பட்ட நாடாக இருப்பினும் — வேறுபாடெதுவும் காட்டப்படுதலாகாது.",
        ]),
        ("உறுப்புரை 3", [
            "வாழ்வதற்கும், சுதந்திரத்திற்கும் பாதுகாப்பிற்கும் சகலரும் உரிமையுடையோராவர்.",
        ]),
    ],
    "bold": "சகலரும்",
    "italic": "சுதந்திரமாகவே",
    "small": "பிறிவியினர்",
    "cases": "(<span>தமி</span><span>ழ்</span>), க<b>ி</b>, க<b>ம</b>ல, நீதி<sup>க</sup>.",
}

MALAYALAM = {
    "title": "മനുഷ്യാവകാശങ്ങളെക്കുറിക്കുന്ന പൊതുപ്രഖ്യാപനം",
    "preamble_title": "പീഠിക",
    "preamble": [
        "മനുഷ്യ സമുദായത്തിന്റെ ജന്മസിദ്ധമായ അന്തസ്സും സമാവകാശവും ലോകത്തില്\u200d സ്വാതന്ത്ര്യം, നീതി, സമാധാനം എന്നിവയുടെ സ്ഥാപനത്തിന്നു അടിസ്ഥാനമായിരിക്കുന്നതിനാലും മനുഷ്യാവകാശങ്ങളെ വകവെക്കാത്തതുകൊണ്ടു മനം മടുപ്പിക്കുന്ന ക്രൂര സംഭവങ്ങളുണ്ടാകുന്നതിനാലും സര്\u200dവ്വതോന്മുഖമായ സ്വാതന്ത്ര്യവും സമൃദ്ധിയും മനുഷ്യനു അനുഭവിക്കാവുന്ന ഒരു പുതു ലോകത്തിന്റെ സ്ഥാപനമാണ്\u200c പൊതുജനങ്ങളുടെ ആഗ്രഹം എന്ന് പ്രഖ്യാപിക്കപ്പെട്ടിരിക്കുന്നതിനാലും ഹിംസാമാര്\u200dഗ്ഗം സ്വീകരിക്കാതിരിക്കണമെങ്കില്\u200d മനുഷ്യാവകാശങ്ങളെ നിയമാനുസൃതമായി വകവെച്ചു കൊടുക്കേണ്ടതാണെന്നുള്ളതിനാലും രാഷ്ട്രങ്ങള്\u200d തമ്മില്\u200d സൌഹൃദം പുലര്\u200dത്തേണ്ടതാണെന്നുള്ളതിനാലും ഐക്യരാഷ്ട്ര ജനത അവരുടെ കരാറില്\u200d സ്ത്രീപുരുഷ സമത്വത്തെക്കുറിച്ചും മനുഷ്യരുടെ മൌലികാവകാശത്തെക്കുറിച്ചും ജീവിതരീതി നന്നാക്കുന്നതിനെക്കുറിച്ചും ഒന്നുകൂടി ഊന്നിപ്പറഞ്ഞിരിക്കുന്നതിനാലും മനുഷ്യാവകാശങ്ങളേയും മൌലിക സ്വാതന്ത്ര്യത്തേയും അന്യോന്യം ബഹുമാനിച്ചുകൊള്ളാമെന്ന് ഐക്യരാഷ്ട്ര സമിതിയിലെ അംഗങ്ങള്\u200d വാഗ്ദാനം ചെയ്തിരിക്കുന്നതിനാലും ഈ അവകാശങ്ങളേയും സ്വാതന്ത്ര്യബോധത്തേയും കുറിച്ചു പൊതുവായി അന്യോന്യം മനസ്സിലാക്കുന്നത്\u200c മേല്\u200dപ്പറഞ്ഞ വാഗ്ദാനത്തെ സഫലമാക്കുന്നതിന്നു അതിപ്രധാനമാണെന്നിരിക്കുന്നതിനാലും ഇപ്പോള്\u200d ജനറല്\u200d അസംബ്ലി (General Assembly) ഇപ്രകാരം പ്രഖ്യാപിക്കുന്നു.",
        "മനുഷ്യാവകാശങ്ങളെ കുറിക്കുന്ന ഈ പൊതുപ്രഖ്യാപനത്തെ ഒരു പ്രമാണമായി കരുതി ഏതൊരു വ്യക്തിക്കും സംഘടനക്കും അവരുടെ പ്രയത്നംകൊണ്ടു മനുഷ്യാവകാശങ്ങളെ ബഹുമാനിച്ചു വകവെച്ചു കൊടുക്കാന്\u200d യത്നിക്കേണ്ടതാണ്\u200c. ക്രമേണ രാഷ്ട്രീയവും അന്തര്\u200dരാഷ്ട്രീയവുമായ പ്രവര്\u200dത്തനങ്ങളെക്കൊണ്ടു ഈ പ്രഖ്യാപനത്തിലടങ്ങിയിരിക്കുന്ന അവകാശങ്ങളെ ഐക്യരാഷ്ട്ര സംഘടനയിലെ അംഗങ്ങളെക്കൊണ്ടും അവരുടെ അധികാരത്തിലിരിക്കുന്ന ജനങ്ങളെക്കൊണ്ടും ഫലപ്രദമാകത്തക്ക രീതിയില്\u200d അംഗീകരിപ്പിക്കുവാന്\u200d ശ്രമിക്കേണ്ടതുമാണ്\u200c.",
    ],
    "articles": [
        ("വകുപ്പ്\u200c 1.", [
            "മനുഷ്യരെല്ലാവരും തുല്യാവകാശങ്ങളോടും അന്തസ്സോടും സ്വാതന്ത്ര്യത്തോടുംകൂടി ജനിച്ചിട്ടുള്ളവരാണ്\u200c. അന്യോന്യം ഭ്രാതൃഭാവത്തോടെ പെരുമാറുവാനാണ്\u200c മനുഷ്യന്നു വിവേകബുദ്ധിയും മനസ്സാക്ഷിയും സിദ്ധമായിരിക്കുന്നത്\u200c.",
        ]),
        ("വകുപ്പ്\u200c 2.", [
            "ജാതി, മതം, നിറം, ഭാഷ, സ്ത്രീപുരുഷഭേദം, രാഷ്ട്രീയാഭിപ്രായം സ്വത്ത്\u200c, കുലം എന്നിവയെ കണക്കാക്കാതെ ഈ പ്രഖ്യാപനത്തില്\u200d പറയുന്ന അവകാശങ്ങള്\u200dക്കും സ്വാതന്ത്ര്യത്തിനും സര്\u200dവ്വജനങ്ങളും അര്\u200dഹരാണ്\u200c. രാഷ്ട്രീയ സ്ഥിതിയെ അടിസ്ഥാനമാക്കി (സ്വതന്ത്രമോ, പരിമിത ഭരണാധികാരത്തോടു കൂടിയതോ ഏതായാലും വേണ്ടതില്ല) ഈ പ്രഖ്യാപനത്തിലെ അവകാശങ്ങളെ സംബന്ധിച്ചേടത്തോളം യാതൊരു വ്യത്യാസവും യാതൊരാളോടും കാണിക്കാന്\u200d പാടുള്ളതല്ല.",
        ]),
        ("വകുപ്പ്\u200c 3.", [
            "സ്വയരക്ഷാബോധത്തോടും സ്വാതന്ത്ര്യത്തോടും കൂടി ജീവിക്കുവാന്\u200d ഏതൊരാള്\u200dക്കും അധികാരമുണ്ട്\u200c.",
        ]),
    ],
    "bold": "അന്തസ്സോടും",
    "italic": "തുല്യാവകാശങ്ങളോടും",
    "small": "ഭ്രാതൃഭാവത്തോടെ",
    "cases": "(<span>മലയാ</span><span>ളം</span>), ക<b>ി</b>, ക<b>മ</b>ലം, നീതി<sup>ക</sup>.",
    # The table: an article's label, and the title as one compound, wider than its cell.
    "table": ("വകുപ്പ്\u200c 1.", "മനുഷ്യാവകാശങ്ങളെക്കുറിക്കുന്നപൊതുപ്രഖ്യാപനം"),
}

ENGLISH = {
    "title": "Universal Declaration of Human Rights",
    "preamble_title": "Preamble",
    "preamble": [
        "Whereas recognition of the inherent dignity and of the equal and inalienable rights of all members of the human family is the foundation of freedom, justice and peace in the world,",
        "Whereas disregard and contempt for human rights have resulted in barbarous acts which have outraged the conscience of mankind, and the advent of a world in which human beings shall enjoy freedom of speech and belief and freedom from fear and want has been proclaimed as the highest aspiration of the common people,",
    ],
    "articles": [
        ("Article 1", [
            "All human beings are born free and equal in dignity and rights. They are endowed with reason and conscience and should act towards one another in a spirit of brotherhood.",
        ]),
        ("Article 2", [
            "Everyone is entitled to all the rights and freedoms set forth in this Declaration, without distinction of any kind, such as race, colour, sex, language, religion, political or other opinion, national or social origin, property, birth or other status.",
            "Furthermore, no distinction shall be made on the basis of the political, jurisdictional or international status of the country or territory to which a person belongs, whether it be independent, trust, non\u2010self\u2010governing or under any other limitation of sovereignty.",
        ]),
        ("Article 3", [
            "Everyone has the right to life, liberty and the security of person.",
        ]),
    ],
}

# Article 1's first sentence in English: the Latin sentence of every book.
LATIN_SENTENCE = "All human beings are born free and equal in dignity and rights."

BOOKS = [
    ("test_shaped_devanagari.epub", "hi", HINDI),
    ("test_shaped_bengali.epub", "bn", BENGALI),
    ("test_shaped_tamil.epub", "ta", TAMIL),
    ("test_shaped_malayalam.epub", "ml", MALAYALAM),
]

LONG_WORDS = 2500

# The bidi controls no source may hold literally (U+200E/F, U+202A-202E, U+2066-2069, U+061C).
BIDI_CONTROLS = {0x200E, 0x200F, 0x061C} | set(range(0x202A, 0x202F)) | set(range(0x2066, 0x206A))


def check_text(text):
    if not unicodedata.is_normalized("NFC", text):
        raise SystemExit(f"not NFC: {text[:40]}")
    if any(ord(c) in BIDI_CONTROLS for c in text):
        raise SystemExit(f"a bidi control in: {text[:40]}")


def wrap(paragraph, word, open_tag, close_tag):
    """`paragraph` with its first word `word` (a whole word, between spaces) inside the tags."""
    words = paragraph.split(" ")
    if word not in words:
        raise SystemExit(f"{word} is not a word of {paragraph[:40]}")
    i = words.index(word)
    words[i] = open_tag + word + close_tag
    return " ".join(words)


def chapter(title, body, lang):
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n<!DOCTYPE html>\n'
        f'<html xmlns="http://www.w3.org/1999/xhtml" xml:lang="{lang}" lang="{lang}">\n'
        f'<head>\n  <title>{title}</title>\n  <link rel="stylesheet" type="text/css" href="style.css"/>\n</head>\n'
        f"<body>\n{body}</body>\n</html>\n"
    )


def opf(title, identifier, lang, chapters):
    items = "".join(
        f'    <item id="ch{i}" href="ch{i}.xhtml" media-type="application/xhtml+xml"/>\n'
        for i in range(1, len(chapters) + 1)
    )
    refs = "".join(f'    <itemref idref="ch{i}"/>\n' for i in range(1, len(chapters) + 1))
    return (
        '<?xml version="1.0" encoding="UTF-8"?>\n'
        '<package xmlns="http://www.idpf.org/2007/opf" version="3.0" unique-identifier="bookid">\n'
        '  <metadata xmlns:dc="http://purl.org/dc/elements/1.1/">\n'
        f"    <dc:title>{title}</dc:title>\n"
        "    <dc:creator>Test Corpus</dc:creator>\n"
        f"    <dc:language>{lang}</dc:language>\n"
        f'    <dc:identifier id="bookid">{identifier}</dc:identifier>\n'
        "  </metadata>\n"
        "  <manifest>\n"
        '    <item id="css" href="style.css" media-type="text/css"/>\n'
        f"{items}"
        "  </manifest>\n"
        f"  <spine>\n{refs}  </spine>\n"
        "</package>\n"
    )


def book_chapters(text, lang):
    for value in [text["title"], text["preamble_title"], *text["preamble"]]:
        check_text(value)
    for title, paragraphs in text["articles"]:
        for value in [title, *paragraphs]:
            check_text(value)
    check_text(text["cases"])

    first = f"<h2>{text['title']}</h2>\n"
    if text["preamble_title"]:
        first += f"<h3>{text['preamble_title']}</h3>\n"
    first += "".join(f"<p>{p}</p>\n" for p in text["preamble"])

    second = ""
    for n, (title, paragraphs) in enumerate(text["articles"]):
        second += f"<h2>{title}</h2>\n"
        for p in paragraphs:
            if n == 0:
                p = wrap(p, text["bold"], "<b>", "</b>")
                p = wrap(p, text["italic"], "<i>", "</i>")
                p = wrap(p, text["small"], '<span style="font-size: 80%">', "</span>")
            second += f"<p>{p}</p>\n"
    second += f"<p>{text['cases']}</p>\n"
    second += f'<p lang="en" xml:lang="en">{LATIN_SENTENCE}</p>\n'
    if "table" in text:
        label, compound = text["table"]
        check_text(label + compound)
        second += f"<table>\n<tr><td>{label}</td><td>{compound}</td></tr>\n</table>\n"
    return [chapter(text["title"], first, lang), chapter(text["articles"][0][0], second, lang)]


def long_chapters(text, lang, paragraph_words=None):
    """The timing book's chapters: a short first, then the declaration's paragraphs, repeated, to LONG_WORDS words.
    With `paragraph_words`, the second holds that many paragraphs of those word counts, the words taken in turn from
    the declaration's."""
    sources = [*text["preamble"], *(p for _, paragraphs in text["articles"] for p in paragraphs)]
    for value in sources:
        check_text(value)
    if paragraph_words is None:
        paragraphs = []
        total = 0
        while total < LONG_WORDS:
            words = sources[len(paragraphs) % len(sources)].split(" ")[: LONG_WORDS - total]
            paragraphs.append(" ".join(words))
            total += len(words)
    else:
        stream = [w for p in sources for w in p.split(" ")]
        paragraphs = []
        taken = 0
        for count in paragraph_words:
            paragraphs.append(" ".join(stream[(taken + k) % len(stream)] for k in range(count)))
            taken += count
    first = f"<h2>{text['title']}</h2>\n<p>{text['preamble'][0]}</p>\n"
    second = "".join(f"<p>{p}</p>\n" for p in paragraphs)
    counts = [len(p.split(" ")) for p in paragraphs]
    return [chapter(text["title"], first, lang), chapter(text["title"], second, lang)], counts


def write(path, files):
    with zipfile.ZipFile(path, "w") as z:
        for name, data, method in files:
            # A fixed date keeps a regenerated book byte-identical, and a fixed system (Unix, as the first books were
            # written on) keeps it so on Windows. The deflated bytes are zlib's: zlib-ng (Python 3.14's) writes
            # others, so regenerate with CPython 3.12.
            info = zipfile.ZipInfo(name, date_time=(2026, 11, 5, 0, 0, 0))
            info.create_system = 3
            info.compress_type = method
            z.writestr(info, data)
    print("wrote", path)


def epub_files(title, identifier, lang, chapters):
    stored, deflated = zipfile.ZIP_STORED, zipfile.ZIP_DEFLATED
    files = [("mimetype", "application/epub+zip", stored), ("META-INF/container.xml", CONTAINER, deflated)]
    files.append(("OEBPS/content.opf", opf(title, identifier, lang, chapters), deflated))
    files.append(("OEBPS/style.css", CSS, deflated))
    files += [(f"OEBPS/ch{i}.xhtml", c, deflated) for i, c in enumerate(chapters, 1)]
    return files


def main():
    # The module's docstring holds Indic text, which a console's code page may not print: --help keeps to ASCII.
    parser = argparse.ArgumentParser(description="Build the four shaped-Indic fixture books (see the script's header).")
    parser.add_argument("--long", metavar="DIR", help="write the two timing books into DIR instead")
    args = parser.parse_args()

    if args.long:
        os.makedirs(args.long, exist_ok=True)
        hindi, counts = long_chapters(HINDI, "hi")
        english, english_counts = long_chapters(ENGLISH, "en", counts)
        assert counts == english_counts and sum(counts) == LONG_WORDS
        write(os.path.join(args.long, "test_shaped_long_hi.epub"),
              epub_files(HINDI["title"], "shaped-long-hi", "hi", hindi))
        write(os.path.join(args.long, "test_shaped_long_en.epub"),
              epub_files(ENGLISH["title"], "shaped-long-en", "en", english))
        print(f"chapter 2: {len(counts)} paragraphs, {sum(counts)} words each")
        return

    for name, lang, text in BOOKS:
        chapters = book_chapters(text, lang)
        write(os.path.join(HERE, name), epub_files(text["title"], name[: -len(".epub")], lang, chapters))


if __name__ == "__main__":
    main()
