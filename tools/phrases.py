# -*- coding: utf-8 -*-
"""A sample sentence per Symbian TLanguage id, in that language's own script.

The engine's text processor is built for one language and one script: giving a
Cyrillic build Latin words (or the reverse) is not a bad reading, it is a
fault. So every probe sentence here is written the way that language is
actually written.
"""

PHRASES = {
    1:   'Hello, this is the Nokia Klatt speech synthesizer speaking British English.',
    2:   "Bonjour, je suis le synthetiseur vocal Nokia Klatt et je parle francais.",
    3:   'Guten Tag, ich bin die Nokia Klatt Sprachausgabe und ich spreche Deutsch.',
    4:   'Hola, soy el sintetizador de voz Nokia Klatt y hablo espanol.',
    5:   'Buongiorno, sono il sintetizzatore vocale Nokia Klatt e parlo italiano.',
    6:   'Hej, jag ar Nokia Klatt talsyntes och jag talar svenska.',
    7:   'Goddag, jeg er Nokia Klatt talesyntese, og jeg taler dansk.',
    8:   'God dag, jeg er Nokia Klatt talesyntese, og jeg snakker norsk.',
    9:   'Hei, mina olen Nokia Klatt puhesyntetisaattori ja puhun suomea.',
    10:  'Hello, this is the Nokia Klatt speech synthesizer speaking American English.',
    13:  'Ola, sou o sintetizador de voz Nokia Klatt e falo portugues.',
    14:  'Merhaba, ben Nokia Klatt konusma sentezleyicisiyim ve Turkce konusuyorum.',
    15:  'Godan dag, eg er Nokia Klatt talgervill og eg tala islensku.',
    16:  'Здравствуйте, '
         'я синтезатор '
         'речи Нокиа Клатт, '
         'и я говорю по-русски.',
    17:  'Jo napot, en vagyok a Nokia Klatt beszedszintetizator es magyarul beszelek.',
    18:  'Goedendag, ik ben de Nokia Klatt spraaksynthese en ik spreek Nederlands.',
    25:  'Dobry den, jsem hlasovy syntezator Nokia Klatt a mluvim cesky.',
    26:  'Dobry den, som hlasovy syntetizator Nokia Klatt a hovorim po slovensky.',
    27:  'Dzien dobry, jestem syntezatorem mowy Nokia Klatt i mowie po polsku.',
    28:  'Dober dan, sem govorni sintetizator Nokia Klatt in govorim slovensko.',
    37:  'مرحبا، أنا مركب '
         'الكلام نوكيا '
         'وأتكلم العربية.',
    39:  'Kumusta, ako ang Nokia Klatt na tagapagsalita at nagsasalita ako ng Tagalog.',
    42:  'Здравейте, аз съм '
         'гласовият синтезатор '
         'Нокиа и говоря '
         'български.',
    44:  'Bon dia, soc el sintetitzador de veu Nokia Klatt i parlo catala.',
    45:  'Dobar dan, ja sam govorni sintetizator Nokia Klatt i govorim hrvatski.',
    49:  'Tere, ma olen Nokia Klatt konesyntesaator ja ma raagin eesti keelt.',
    51:  'Bonjour, je suis le synthetiseur vocal Nokia Klatt et je parle francais canadien.',
    54:  'Γεια σας, είμαι ο '
         'συνθετής φωνής '
         'Νοκια και μιλάω '
         'ελληνικά.',
    57:  'שלום, אני מסנתז '
         'הדיבור של נוקיה '
         'ואני מדבר עברית.',
    67:  'Sveiki, es esmu Nokia Klatt runas sintezators un es runaju latviski.',
    68:  'Sveiki, as esu Nokia Klatt kalbos sintezatorius ir kalbu lietuviskai.',
    76:  'Ola, eu sou o sintetizador de voz Nokia Klatt e falo portugues do Brasil.',
    78:  'Buna ziua, sunt sintetizatorul de voce Nokia Klatt si vorbesc romaneste.',
    79:  'Добар дан, ја сам '
         'говорни синтетизатор '
         'Нокиа и говорим '
         'српски.',
    83:  'Hola, soy el sintetizador de voz Nokia Klatt y hablo espanol latinoamericano.',
    93:  'Доброго дня, я '
         'синтезатор мовлення '
         'Нокіа і я говорю '
         'українською.',
    96:  'Xin chao, toi la bo tong hop giong noi Nokia Klatt va toi noi tieng Viet.',
    401: 'Kaixo, Nokia Klatt ahots sintetizatzailea naiz eta euskaraz hitz egiten dut.',
    402: 'Ola, son o sintetizador de voz Nokia Klatt e falo galego.',
}

# Anything not in the table gets a sentence in English; a digit is the only
# text every processor is guaranteed to accept, so it goes first.
FALLBACK = '1 2 3. Nokia Klatt speech synthesizer.'


def phrase(lang_id):
    return PHRASES.get(lang_id, FALLBACK)
