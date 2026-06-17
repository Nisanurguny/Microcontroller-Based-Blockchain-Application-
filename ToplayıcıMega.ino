#include <Arduino.h>
#include <LiquidCrystal.h>

// LCD pinleri: RS, EN, D4, D5, D6, D7
LiquidCrystal lcd(7, 8, 9, 10, 11, 12);

// ------------------------------------------------------
// SON MEGA RX AYARI
// ------------------------------------------------------
// D22 = PA0 -> Ara Mega 1
// D23 = PA1 -> Ara Mega 2

const uint8_t STX = 0x02;
const uint8_t ETX = 0x03;

const uint32_t ARA_MEGA_BAUD_RATE = 4800;
const uint16_t BIT_SURE_US = 1000000UL / ARA_MEGA_BAUD_RATE;        // 208 us
const uint16_t ILK_ORNEKLEME_US = BIT_SURE_US + (BIT_SURE_US / 2);  // 312 us

const uint32_t GENEL_TIMEOUT_MS = 30000;

const uint16_t YUK_BOYUTU = 512;
const uint16_t TOPLAM_VERI_BOYUTU = 4096; // 8 x 512

// ------------------------------------------------------
// VERI PAKETI
// ------------------------------------------------------
#pragma pack(push, 1)
struct VeriPaketi {
  uint8_t parca_numarasi;
  uint8_t katman_numarasi;
  uint32_t zaman_damgasi;
  uint16_t veri_uzunlugu;
  uint8_t yuk_verisi[512];
  uint32_t onceki_crc;
  uint32_t mevcut_crc;
};
#pragma pack(pop)

// RAM tasmasin diye sadece tek paket deposu kullaniyoruz.
// 8 x 527 byte ≈ 4216 byte
VeriPaketi paketDeposu[8];

// parcaHaritasi[parcaNo] = paketDeposu icindeki index
uint8_t parcaHaritasi[8];

bool parcaAlindi[8];
bool araCerceveAlindi[2];

// ------------------------------------------------------
// RX VE PARSER DURUMLARI
// ------------------------------------------------------
enum RxDurumu {
  RX_START_BEKLE,
  RX_DATA_OKU,
  RX_STOP_OKU
};

enum ParserDurumu {
  PARSER_STX_BEKLE,
  PARSER_ARA_NO_OKU,
  PARSER_PAKET_SAYISI_OKU,
  PARSER_PAKETLERI_OKU,
  PARSER_ETX_BEKLE,
  PARSER_TAMAMLANDI
};

struct KanalDurumu {
  uint8_t kanalNo;      // 0 veya 1
  uint8_t pinNo;        // D22 veya D23
  uint8_t maske;        // PA0 veya PA1

  RxDurumu rxDurumu;
  ParserDurumu parserDurumu;

  uint8_t bitNo;
  uint8_t okunanByte;
  uint32_t sonrakiOrneklemeZamani;

  uint8_t araMegaNo;
  uint8_t paketSayisi;

  uint8_t paketSirasi;
  uint16_t paketByteIndex;

  uint32_t okunanByteSayisi;
  uint16_t stxSayisi;
  uint16_t etxSayisi;
  uint16_t hataSayisi;

  bool cerceveTamam;
};

KanalDurumu kanallar[2];

// ------------------------------------------------------
// CRC32
// ------------------------------------------------------
uint32_t crc32Hesapla(const uint8_t *veri, size_t uzunluk) {
  uint32_t crc_degeri = 0xFFFFFFFF;

  for (size_t i = 0; i < uzunluk; i++) {
    crc_degeri ^= veri[i];

    for (uint8_t j = 0; j < 8; j++) {
      if (crc_degeri & 1) {
        crc_degeri = (crc_degeri >> 1) ^ 0xEDB88320;
      } else {
        crc_degeri >>= 1;
      }
    }
  }

  return ~crc_degeri;
}

// ------------------------------------------------------
// HEX YARDIMCI
// ------------------------------------------------------
char hexKarakter(uint8_t deger) {
  deger &= 0x0F;

  if (deger < 10) {
    return '0' + deger;
  }

  return 'A' + (deger - 10);
}

void byteHexYazSerial(uint8_t veri) {
  Serial.print(hexKarakter(veri >> 4));
  Serial.print(hexKarakter(veri));
}

void uint32HexYazSerial(uint32_t veri) {
  Serial.print(F("0x"));

  for (int8_t i = 7; i >= 0; i--) {
    uint8_t nibble = (veri >> (i * 4)) & 0x0F;
    Serial.print(hexKarakter(nibble));
  }
}

// ------------------------------------------------------
// LCD
// ------------------------------------------------------
void lcdDurumYaz(const __FlashStringHelper* ust, const __FlashStringHelper* alt) {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(ust);

  lcd.setCursor(0, 1);
  lcd.print(alt);
}

// ------------------------------------------------------
// KANAL SIFIRLA
// ------------------------------------------------------
void kanalSifirla(uint8_t kanal) {
  kanallar[kanal].kanalNo = kanal;
  kanallar[kanal].pinNo = 22 + kanal;
  kanallar[kanal].maske = (1 << kanal);

  kanallar[kanal].rxDurumu = RX_START_BEKLE;
  kanallar[kanal].parserDurumu = PARSER_STX_BEKLE;

  kanallar[kanal].bitNo = 0;
  kanallar[kanal].okunanByte = 0;
  kanallar[kanal].sonrakiOrneklemeZamani = 0;

  kanallar[kanal].araMegaNo = 0;
  kanallar[kanal].paketSayisi = 0;

  kanallar[kanal].paketSirasi = 0;
  kanallar[kanal].paketByteIndex = 0;

  kanallar[kanal].okunanByteSayisi = 0;
  kanallar[kanal].stxSayisi = 0;
  kanallar[kanal].etxSayisi = 0;
  kanallar[kanal].hataSayisi = 0;

  kanallar[kanal].cerceveTamam = false;

  araCerceveAlindi[kanal] = false;
}

// ------------------------------------------------------
// SISTEM SIFIRLA
// ------------------------------------------------------
void sistemiSifirla() {
  for (uint8_t i = 0; i < 2; i++) {
    kanalSifirla(i);
  }

  memset(paketDeposu, 0, sizeof(paketDeposu));

  for (uint8_t i = 0; i < 8; i++) {
    parcaHaritasi[i] = 255;
    parcaAlindi[i] = false;
  }
}

// ------------------------------------------------------
// PARSER
// Beklenen cerceve:
// STX + ARA_MEGA_NO + PAKET_SAYISI + 4 x VeriPaketi + ETX
// ------------------------------------------------------
void parserByteIsle(uint8_t kanal, uint8_t gelenByte) {
  KanalDurumu &k = kanallar[kanal];

  if (k.parserDurumu == PARSER_TAMAMLANDI) {
    return;
  }

  if (k.parserDurumu == PARSER_STX_BEKLE) {
    if (gelenByte == STX) {
      k.stxSayisi++;
      k.parserDurumu = PARSER_ARA_NO_OKU;
    }

    return;
  }

  if (k.parserDurumu == PARSER_ARA_NO_OKU) {
    k.araMegaNo = gelenByte;

    if (k.araMegaNo == 1 || k.araMegaNo == 2) {
      k.parserDurumu = PARSER_PAKET_SAYISI_OKU;
    } else {
      k.hataSayisi++;
      k.parserDurumu = PARSER_STX_BEKLE;
    }

    return;
  }

  if (k.parserDurumu == PARSER_PAKET_SAYISI_OKU) {
    k.paketSayisi = gelenByte;

    if (k.paketSayisi == 4) {
      k.paketSirasi = 0;
      k.paketByteIndex = 0;
      k.parserDurumu = PARSER_PAKETLERI_OKU;
    } else {
      k.hataSayisi++;
      k.parserDurumu = PARSER_STX_BEKLE;
    }

    return;
  }

  if (k.parserDurumu == PARSER_PAKETLERI_OKU) {
    // Her kanal 4 paket getirir.
    // Kanal 0 -> paketDeposu[0..3]
    // Kanal 1 -> paketDeposu[4..7]
    uint8_t depoIndex = kanal * 4 + k.paketSirasi;

    uint8_t* hedefBellek = (uint8_t*)&paketDeposu[depoIndex];

    hedefBellek[k.paketByteIndex] = gelenByte;
    k.paketByteIndex++;

    if (k.paketByteIndex >= sizeof(VeriPaketi)) {
      k.paketByteIndex = 0;
      k.paketSirasi++;

      if (k.paketSirasi >= 4) {
        k.parserDurumu = PARSER_ETX_BEKLE;
      }
    }

    return;
  }

  if (k.parserDurumu == PARSER_ETX_BEKLE) {
    if (gelenByte == ETX) {
      k.etxSayisi++;
      k.cerceveTamam = true;
      araCerceveAlindi[kanal] = true;
      k.parserDurumu = PARSER_TAMAMLANDI;
    } else {
      k.hataSayisi++;

      k.parserDurumu = PARSER_STX_BEKLE;
      k.araMegaNo = 0;
      k.paketSayisi = 0;
      k.paketSirasi = 0;
      k.paketByteIndex = 0;

      for (uint8_t i = 0; i < 4; i++) {
        uint8_t depoIndex = kanal * 4 + i;
        memset(&paketDeposu[depoIndex], 0, sizeof(VeriPaketi));
      }
    }

    return;
  }
}

// ------------------------------------------------------
// D22 VE D23'U AYNI ANDA TARA
// ------------------------------------------------------
void ikiKanaliAyniAndaTara() {
  uint32_t simdi = micros();
  uint8_t portDurumu = PINA & 0x03;

  for (uint8_t kanal = 0; kanal < 2; kanal++) {
    KanalDurumu &k = kanallar[kanal];

    if (k.cerceveTamam) {
      continue;
    }

    // START BIT BEKLE
    // Seri hat bosta HIGH, start bit LOW olur.
    if (k.rxDurumu == RX_START_BEKLE) {
      if ((portDurumu & k.maske) == 0) {
        k.rxDurumu = RX_DATA_OKU;
        k.bitNo = 0;
        k.okunanByte = 0;

        // 4800 baud icin ilk data bit merkezi yaklasik 312 us sonra
        k.sonrakiOrneklemeZamani = simdi + ILK_ORNEKLEME_US;
      }

      continue;
    }

    // DATA BITLERINI OKU
    if (k.rxDurumu == RX_DATA_OKU) {
      if ((int32_t)(simdi - k.sonrakiOrneklemeZamani) >= 0) {
        uint8_t anlikPort = PINA & 0x03;

        if (anlikPort & k.maske) {
          k.okunanByte |= (1 << k.bitNo);
        }

        k.bitNo++;
        k.sonrakiOrneklemeZamani += BIT_SURE_US;

        if (k.bitNo >= 8) {
          k.rxDurumu = RX_STOP_OKU;
        }
      }

      continue;
    }

    // STOP BIT OKU
    if (k.rxDurumu == RX_STOP_OKU) {
      if ((int32_t)(simdi - k.sonrakiOrneklemeZamani) >= 0) {
        uint8_t anlikPort = PINA & 0x03;

        // Stop bit HIGH olmali
        if ((anlikPort & k.maske) == 0) {
          k.hataSayisi++;
        }

        k.okunanByteSayisi++;

        parserByteIsle(kanal, k.okunanByte);

        k.rxDurumu = RX_START_BEKLE;
        k.bitNo = 0;
        k.okunanByte = 0;
      }

      continue;
    }
  }
}

// ------------------------------------------------------
// IKI ARA MEGA'DAN CERCEVE GELDI MI?
// ------------------------------------------------------
bool ikiCerceveTamamMi() {
  return araCerceveAlindi[0] && araCerceveAlindi[1];
}

// ------------------------------------------------------
// PARCA HARITASI OLUSTUR
// paketDeposu icindeki paketleri parca_numarasi'na gore eslestirir.
// ------------------------------------------------------
void parcaHaritasiOlustur() {
  for (uint8_t i = 0; i < 8; i++) {
    parcaHaritasi[i] = 255;
    parcaAlindi[i] = false;
  }

  for (uint8_t depoIndex = 0; depoIndex < 8; depoIndex++) {
    uint8_t parca = paketDeposu[depoIndex].parca_numarasi;

    if (parca < 8) {
      parcaHaritasi[parca] = depoIndex;
      parcaAlindi[parca] = true;
    }
  }
}

// ------------------------------------------------------
// 8 PARCA TAMAM MI?
// ------------------------------------------------------
bool sekizParcaTamamMi() {
  for (uint8_t i = 0; i < 8; i++) {
    if (!parcaAlindi[i]) {
      return false;
    }
  }

  return true;
}

// ------------------------------------------------------
// BIRLESIK VERIDEN BYTE GETIR
// RAM'de 4096 byte ayri dizi tutmuyoruz.
// Dogrudan ilgili paketin yuk_verisi icinden okuyoruz.
// ------------------------------------------------------
uint8_t birlesikVeriByteGetir(uint16_t globalIndex) {
  uint8_t parca = globalIndex / 512;
  uint16_t offset = globalIndex % 512;

  if (parca >= 8) {
    return 0;
  }

  if (!parcaAlindi[parca]) {
    return 0;
  }

  uint8_t depoIndex = parcaHaritasi[parca];

  if (depoIndex >= 8) {
    return 0;
  }

  return paketDeposu[depoIndex].yuk_verisi[offset];
}

// ------------------------------------------------------
// LCD'DE BIRLESIK SIFRELI VERIYI HEX OLARAK KAYDIR
// ------------------------------------------------------
uint32_t toplamHexKarakterSayisi() {
  return 4096UL * 3UL; // 2 HEX + 1 bosluk
}

char sifreKarakteriGetir(uint32_t pozisyon) {
  uint32_t toplam = toplamHexKarakterSayisi();
  pozisyon = pozisyon % toplam;

  uint16_t byteSirasi = pozisyon / 3;
  uint8_t byteIci = pozisyon % 3;

  if (byteIci == 2) {
    return ' ';
  }

  uint8_t veri = birlesikVeriByteGetir(byteSirasi);

  if (byteIci == 0) {
    return hexKarakter(veri >> 4);
  }

  return hexKarakter(veri);
}

void lcdSifreliVeriGoster(uint32_t sureMs) {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(F("Tek SifreliVeri"));

  uint32_t baslangic = millis();
  uint32_t index = 0;

  while (millis() - baslangic < sureMs) {
    char satir[17];

    for (uint8_t i = 0; i < 16; i++) {
      satir[i] = sifreKarakteriGetir(index + i);
    }

    satir[16] = '\0';

    lcd.setCursor(0, 1);
    lcd.print(satir);

    index++;

    if (index >= toplamHexKarakterSayisi()) {
      index = 0;
    }

    delay(250);
  }
}

// ------------------------------------------------------
// SERIAL RAPOR
// ------------------------------------------------------
void raporBas() {
  Serial.println();
  Serial.println(F("=================================================="));
  Serial.println(F("SON MEGA RAPORU - RAM OPTIMIZE"));
  Serial.println(F("=================================================="));

  Serial.print(F("Ara Mega baud rate: "));
  Serial.println(ARA_MEGA_BAUD_RATE);

  Serial.print(F("Bit suresi us: "));
  Serial.println(BIT_SURE_US);

  Serial.print(F("Ilk ornekleme us: "));
  Serial.println(ILK_ORNEKLEME_US);

  Serial.print(F("VeriPaketi boyutu: "));
  Serial.println(sizeof(VeriPaketi));

  Serial.println();

  for (uint8_t kanal = 0; kanal < 2; kanal++) {
    Serial.print(F("KANAL D"));
    Serial.print(22 + kanal);
    Serial.println(F(":"));

    Serial.print(F("  Cerceve alindi: "));
    Serial.println(araCerceveAlindi[kanal] ? F("EVET") : F("HAYIR"));

    Serial.print(F("  Ara Mega No: "));
    Serial.println(kanallar[kanal].araMegaNo);

    Serial.print(F("  Paket sayisi: "));
    Serial.println(kanallar[kanal].paketSayisi);

    Serial.print(F("  Okunan byte sayisi: "));
    Serial.println(kanallar[kanal].okunanByteSayisi);

    Serial.print(F("  STX sayisi: "));
    Serial.println(kanallar[kanal].stxSayisi);

    Serial.print(F("  ETX sayisi: "));
    Serial.println(kanallar[kanal].etxSayisi);

    Serial.print(F("  Hata sayisi: "));
    Serial.println(kanallar[kanal].hataSayisi);

    Serial.println();
  }

  Serial.println(F("PARCA RAPORU:"));

  for (uint8_t parca = 0; parca < 8; parca++) {
    Serial.print(F("Parca "));
    Serial.print(parca);
    Serial.println(F(":"));

    Serial.print(F("  Alindi: "));
    Serial.println(parcaAlindi[parca] ? F("EVET") : F("HAYIR"));

    if (!parcaAlindi[parca]) {
      Serial.println();
      continue;
    }

    uint8_t depoIndex = parcaHaritasi[parca];
    VeriPaketi &p = paketDeposu[depoIndex];

    uint32_t hesaplananCrc = crc32Hesapla(p.yuk_verisi, 512);
    bool crcDogru = (hesaplananCrc == p.mevcut_crc);

    Serial.print(F("  Depo index: "));
    Serial.println(depoIndex);

    Serial.print(F("  Paket parca no: "));
    Serial.println(p.parca_numarasi);

    Serial.print(F("  Katman no: "));
    Serial.println(p.katman_numarasi);

    Serial.print(F("  Veri uzunlugu: "));
    Serial.println(p.veri_uzunlugu);

    Serial.print(F("  Mevcut CRC: "));
    uint32HexYazSerial(p.mevcut_crc);
    Serial.println();

    Serial.print(F("  Hesaplanan CRC: "));
    uint32HexYazSerial(hesaplananCrc);
    Serial.println();

    Serial.print(F("  CRC durumu: "));
    Serial.println(crcDogru ? F("DOGRU") : F("HATALI"));

    Serial.print(F("  Ilk 16 byte HEX: "));
    for (uint8_t i = 0; i < 16; i++) {
      byteHexYazSerial(p.yuk_verisi[i]);
      Serial.print(' ');
    }

    Serial.println();
    Serial.println();
  }

  Serial.println(F("BIRLESIK TEK SIFRELI VERI HEX BASLANGIC"));
  Serial.println(F("--------------------------------------------------"));

  for (uint16_t i = 0; i < TOPLAM_VERI_BOYUTU; i++) {
    byteHexYazSerial(birlesikVeriByteGetir(i));
    Serial.print(' ');

    if ((i + 1) % 32 == 0) {
      Serial.println();
    }
  }

  Serial.println();
  Serial.println(F("--------------------------------------------------"));
  Serial.println(F("BIRLESIK TEK SIFRELI VERI HEX BITIS"));
  Serial.println(F("=================================================="));
}

// ------------------------------------------------------
// SETUP
// ------------------------------------------------------
void setup() {
  Serial.begin(115200);

  lcd.begin(16, 2);
  lcd.clear();

  // D22-D23 giris
  DDRA &= ~0x03;

  // D22-D23 pull-up aktif
  PORTA |= 0x03;

  sistemiSifirla();

  lcdDurumYaz(F("Son Mega Hazir"), F("D22-D23 RX"));

  Serial.println(F("Son Mega hazir."));
  Serial.println(F("D22 = Ara Mega 1 RX"));
  Serial.println(F("D23 = Ara Mega 2 RX"));
  Serial.print(F("Ara Mega baud rate: "));
  Serial.println(ARA_MEGA_BAUD_RATE);
  Serial.println(F("Serial Monitor: 115200 baud"));
}

// ------------------------------------------------------
// LOOP
// ------------------------------------------------------
void loop() {
  sistemiSifirla();

  lcdDurumYaz(F("D22-D23 RX"), F("Veri bekleniyor"));

  Serial.println();
  Serial.println(F("D22 ve D23 uzerinden iki Ara Mega verisi bekleniyor..."));

  uint32_t baslangic = millis();

  // Veri alma sirasinda LCD ve Serial yazdirma yok.
  while (!ikiCerceveTamamMi() && (millis() - baslangic < GENEL_TIMEOUT_MS)) {
    ikiKanaliAyniAndaTara();
  }

  if (ikiCerceveTamamMi()) {
    parcaHaritasiOlustur();

    if (sekizParcaTamamMi()) {
      lcdDurumYaz(F("8 Parca Alindi"), F("Tek veri hazir"));

      delay(1000);

      // LCD'de tek birleşik şifreli veri HEX olarak gösterilir
      lcdSifreliVeriGoster(8000);

      // PC Serial Monitor raporu
      raporBas();

      lcdDurumYaz(F("Rapor Bitti"), F("Tekrar bekle"));

      delay(2000);
    } else {
      lcdDurumYaz(F("Parca Eksik"), F("Serial bak"));

      Serial.println();
      Serial.println(F("UYARI: Iki Ara Mega cercevesi geldi ama 8 parcanin tamami tamamlanmadi."));

      for (uint8_t i = 0; i < 8; i++) {
        Serial.print(F("Parca "));
        Serial.print(i);
        Serial.print(F(": "));
        Serial.println(parcaAlindi[i] ? F("EVET") : F("HAYIR"));
      }

      delay(3000);
    }
  } else {
    lcdDurumYaz(F("Timeout/Hata"), F("Serial bak"));

    Serial.println();
    Serial.println(F("UYARI: D22/D23 cerceveleri zamaninda tamamlanamadi."));

    for (uint8_t kanal = 0; kanal < 2; kanal++) {
      Serial.print(F("D"));
      Serial.print(22 + kanal);

      Serial.print(F(" | Cerceve="));
      Serial.print(araCerceveAlindi[kanal] ? F("EVET") : F("HAYIR"));

      Serial.print(F(" | AraNo="));
      Serial.print(kanallar[kanal].araMegaNo);

      Serial.print(F(" | Byte="));
      Serial.print(kanallar[kanal].okunanByteSayisi);

      Serial.print(F(" | STX="));
      Serial.print(kanallar[kanal].stxSayisi);

      Serial.print(F(" | ETX="));
      Serial.print(kanallar[kanal].etxSayisi);

      Serial.print(F(" | Hata="));
      Serial.println(kanallar[kanal].hataSayisi);
    }

    delay(3000);
  }
}