#include <Arduino.h>
#include <LiquidCrystal.h>

// LCD pinleri: RS, EN, D4, D5, D6, D7
LiquidCrystal lcd(7, 8, 9, 10, 11, 12);

// ------------------------------------------------------
// ARA MEGA AYARI
// ------------------------------------------------------
// Ara Mega 1: 0,1,2,3 numarali Unolar
// Ara Mega 2: 4,5,6,7 numarali Unolar
const uint8_t ARA_MEGA_NO = 2;

// ------------------------------------------------------
// 4800 BAUD AYARI
// ------------------------------------------------------
const uint32_t UNO_BAUD_RATE = 4800;
const uint16_t BIT_SURE_US = 1000000UL / UNO_BAUD_RATE;              // 208 us
const uint16_t ILK_ORNEKLEME_US = BIT_SURE_US + BIT_SURE_US / 2;     // 312 us

// D22 = PA0 RX
// D23 = PA1 RX
// D24 = PA2 RX
// D25 = PA3 RX
// D28 = PA6 TX

const uint8_t KANAL_SAYISI = 4;

const uint8_t STX = 0x02;
const uint8_t ETX = 0x03;

const uint32_t GENEL_TIMEOUT_MS = 15000;

// ------------------------------------------------------
// D28 SANAL TX AYARI
// ------------------------------------------------------
const uint8_t TX_D28_BIT = 6;                 // D28 = PA6
const uint8_t TX_D28_MASK = (1 << TX_D28_BIT);

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

VeriPaketi gelenPaketler[4];
uint8_t birlesikVeri[2048];

bool paketAlindi[4] = { false, false, false, false };

// ------------------------------------------------------
// RX DURUMLARI
// ------------------------------------------------------
enum RxDurumu {
  RX_START_BEKLE,
  RX_DATA_OKU,
  RX_STOP_OKU
};

enum ParserDurumu {
  PARSER_STX_BEKLE,
  PARSER_PAKET_OKU,
  PARSER_ETX_BEKLE,
  PARSER_TAMAMLANDI
};

struct KanalDurumu {
  uint8_t kanalNo;
  uint8_t pinNo;
  uint8_t maske;

  RxDurumu rxDurumu;
  ParserDurumu parserDurumu;

  uint8_t bitNo;
  uint8_t okunanByte;

  uint32_t sonrakiOrneklemeZamani;

  uint16_t paketIndex;

  uint32_t okunanByteSayisi;
  uint32_t stxSayisi;
  uint32_t etxSayisi;
  uint32_t hataSayisi;

  bool paketTamam;
};

KanalDurumu kanallar[4];

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
// HEX
// ------------------------------------------------------
char hexKarakter(uint8_t deger) {
  deger &= 0x0F;

  if (deger < 10) return '0' + deger;

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
void lcdDurumYaz(const char* ust, const char* alt) {
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print(ust);

  lcd.setCursor(0, 1);
  lcd.print(alt);
}

// ------------------------------------------------------
// D28 SANAL TX FONKSIYONLARI
// ------------------------------------------------------
void sanalTxHazirla() {
  // D28 = PA6 cikis
  DDRA |= TX_D28_MASK;

  // Seri hat bosta HIGH olur
  PORTA |= TX_D28_MASK;
}

void sanalTxBitYaz(bool seviye) {
  if (seviye) {
    PORTA |= TX_D28_MASK;
  } else {
    PORTA &= ~TX_D28_MASK;
  }
}

void sanalTxByteGonder(uint8_t veri) {
  noInterrupts();

  // Start bit = LOW
  sanalTxBitYaz(false);
  delayMicroseconds(BIT_SURE_US);

  // 8 data bit, LSB-first
  for (uint8_t bitNo = 0; bitNo < 8; bitNo++) {
    bool bitDegeri = veri & (1 << bitNo);
    sanalTxBitYaz(bitDegeri);
    delayMicroseconds(BIT_SURE_US);
  }

  // Stop bit = HIGH
  sanalTxBitYaz(true);
  delayMicroseconds(BIT_SURE_US);

  interrupts();
}

void sanalTxBufferGonder(const uint8_t* veri, uint16_t uzunluk) {
  for (uint16_t i = 0; i < uzunluk; i++) {
    sanalTxByteGonder(veri[i]);
  }
}

// Parca numarasina gore 4 paketi dogru sirayla gondermek icin
uint8_t paketKanalIndexBul(uint8_t yerelIndex) {
  uint8_t beklenenBaslangic = (ARA_MEGA_NO == 1) ? 0 : 4;
  uint8_t hedefParca = beklenenBaslangic + yerelIndex;

  for (uint8_t kanal = 0; kanal < 4; kanal++) {
    if (gelenPaketler[kanal].parca_numarasi == hedefParca) {
      return kanal;
    }
  }

  // Parca numarasi beklenen gibi degilse fiziksel kanal sirasini kullan
  return yerelIndex;
}

// Son Mega'nin bekledigi cerceve:
// STX + ARA_MEGA_NO + 4 + 4 adet VeriPaketi + ETX
void sonMegayaCerceveGonder() {
  uint16_t toplamByte = 4 + (sizeof(VeriPaketi) * 4);

  Serial.println();
  Serial.println(F("Son Mega'ya D28 uzerinden veri gonderiliyor..."));
  Serial.print(F("Gonderilecek toplam byte: "));
  Serial.println(toplamByte);
  Serial.print(F("Ara Mega No: "));
  Serial.println(ARA_MEGA_NO);

  // Hat bosta HIGH durumda biraz beklesin
  sanalTxBitYaz(true);
  delay(20);

  // STX
  sanalTxByteGonder(STX);

  // Ara Mega No
  sanalTxByteGonder(ARA_MEGA_NO);

  // Paket sayisi
  sanalTxByteGonder(4);

  // 4 adet VeriPaketi
  for (uint8_t yerelIndex = 0; yerelIndex < 4; yerelIndex++) {
    uint8_t kanalIndex = paketKanalIndexBul(yerelIndex);
    uint8_t* paketBellek = (uint8_t*)&gelenPaketler[kanalIndex];

    sanalTxBufferGonder(paketBellek, sizeof(VeriPaketi));
  }

  // ETX
  sanalTxByteGonder(ETX);

  // Hat tekrar HIGH
  sanalTxBitYaz(true);

  Serial.println(F("Son Mega'ya gonderim tamamlandi."));
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

  kanallar[kanal].paketIndex = 0;

  kanallar[kanal].okunanByteSayisi = 0;
  kanallar[kanal].stxSayisi = 0;
  kanallar[kanal].etxSayisi = 0;
  kanallar[kanal].hataSayisi = 0;

  kanallar[kanal].paketTamam = false;

  paketAlindi[kanal] = false;

  memset(&gelenPaketler[kanal], 0, sizeof(VeriPaketi));
}

void sistemiSifirla() {
  for (uint8_t i = 0; i < 4; i++) {
    kanalSifirla(i);
  }

  memset(birlesikVeri, 0, sizeof(birlesikVeri));
}

// ------------------------------------------------------
// PARSER
// ------------------------------------------------------
void parserByteIsle(uint8_t kanal, uint8_t gelenByte) {
  KanalDurumu &k = kanallar[kanal];

  if (k.parserDurumu == PARSER_TAMAMLANDI) {
    return;
  }

  if (k.parserDurumu == PARSER_STX_BEKLE) {
    if (gelenByte == STX) {
      k.stxSayisi++;
      k.paketIndex = 0;
      k.parserDurumu = PARSER_PAKET_OKU;
    }

    return;
  }

  if (k.parserDurumu == PARSER_PAKET_OKU) {
    uint8_t* hedefBellek = (uint8_t*)&gelenPaketler[kanal];

    hedefBellek[k.paketIndex] = gelenByte;
    k.paketIndex++;

    if (k.paketIndex >= sizeof(VeriPaketi)) {
      k.parserDurumu = PARSER_ETX_BEKLE;
    }

    return;
  }

  if (k.parserDurumu == PARSER_ETX_BEKLE) {
    if (gelenByte == ETX) {
      k.etxSayisi++;
      k.paketTamam = true;
      paketAlindi[kanal] = true;
      k.parserDurumu = PARSER_TAMAMLANDI;
    } else {
      k.hataSayisi++;

      k.parserDurumu = PARSER_STX_BEKLE;
      k.paketIndex = 0;
      memset(&gelenPaketler[kanal], 0, sizeof(VeriPaketi));
    }

    return;
  }
}

// ------------------------------------------------------
// 4 KANALI 4800 BAUD'A GORE AYNI ANDA TARA
// ------------------------------------------------------
void dortKanaliAyniAndaTara() {
  uint32_t simdi = micros();
  uint8_t portDurumu = PINA & 0x0F;

  for (uint8_t kanal = 0; kanal < 4; kanal++) {
    KanalDurumu &k = kanallar[kanal];

    if (k.paketTamam) {
      continue;
    }

    // START BIT BEKLE
    // Bos seri hat HIGH, start bit LOW olur
    if (k.rxDurumu == RX_START_BEKLE) {
      if ((portDurumu & k.maske) == 0) {
        k.rxDurumu = RX_DATA_OKU;
        k.bitNo = 0;
        k.okunanByte = 0;

        // 4800 baud icin ilk data bit merkezi:
        // start bit yakalandiktan yaklasik 312 us sonra
        k.sonrakiOrneklemeZamani = simdi + ILK_ORNEKLEME_US;
      }

      continue;
    }

    // DATA BIT OKU
    if (k.rxDurumu == RX_DATA_OKU) {
      if ((int32_t)(simdi - k.sonrakiOrneklemeZamani) >= 0) {
        uint8_t anlikPort = PINA & 0x0F;

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
        uint8_t anlikPort = PINA & 0x0F;

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
// 4 PAKET TAMAM MI?
// ------------------------------------------------------
bool dortPaketTamamMi() {
  for (uint8_t i = 0; i < 4; i++) {
    if (!paketAlindi[i]) {
      return false;
    }
  }

  return true;
}

// ------------------------------------------------------
// PAKETLERI BIRLESTIR
// ------------------------------------------------------
void paketleriBirlestir() {
  memset(birlesikVeri, 0, sizeof(birlesikVeri));

  uint8_t beklenenBaslangic = (ARA_MEGA_NO == 1) ? 0 : 4;

  for (uint8_t kanal = 0; kanal < 4; kanal++) {
    uint8_t parca = gelenPaketler[kanal].parca_numarasi;
    uint8_t yerelIndex = kanal;

    if (parca >= beklenenBaslangic && parca < beklenenBaslangic + 4) {
      yerelIndex = parca - beklenenBaslangic;
    }

    memcpy(&birlesikVeri[yerelIndex * 512], gelenPaketler[kanal].yuk_verisi, 512);
  }
}

// ------------------------------------------------------
// LCD'DE SIFRELI VERIYI HEX OLARAK GOSTER
// ------------------------------------------------------
uint32_t toplamHexKarakterSayisi() {
  return 2048UL * 3UL;
}

char sifreKarakteriGetir(uint32_t pozisyon) {
  uint32_t toplam = toplamHexKarakterSayisi();
  pozisyon = pozisyon % toplam;

  uint32_t byteSirasi = pozisyon / 3;
  uint8_t byteIci = pozisyon % 3;

  if (byteIci == 2) return ' ';

  uint8_t veri = birlesikVeri[byteSirasi];

  if (byteIci == 0) return hexKarakter(veri >> 4);

  return hexKarakter(veri);
}

void lcdSifreliVeriGoster(uint32_t sureMs) {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Sifreli Veri");

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
  Serial.print(F("ARA MEGA "));
  Serial.print(ARA_MEGA_NO);
  Serial.println(F(" 4800 BAUD 4 KANAL RX RAPORU"));
  Serial.println(F("=================================================="));

  Serial.print(F("UNO baud rate: "));
  Serial.println(UNO_BAUD_RATE);

  Serial.print(F("Bit suresi us: "));
  Serial.println(BIT_SURE_US);

  Serial.print(F("Ilk ornekleme us: "));
  Serial.println(ILK_ORNEKLEME_US);

  Serial.print(F("VeriPaketi boyutu: "));
  Serial.println(sizeof(VeriPaketi));

  uint8_t beklenenBaslangic = (ARA_MEGA_NO == 1) ? 0 : 4;

  Serial.print(F("Beklenen parca araligi: "));
  Serial.print(beklenenBaslangic);
  Serial.print(F("-"));
  Serial.println(beklenenBaslangic + 3);

  Serial.println();

  for (uint8_t kanal = 0; kanal < 4; kanal++) {
    uint32_t hesaplananCrc = crc32Hesapla(gelenPaketler[kanal].yuk_verisi, 512);
    bool crcDogru = (hesaplananCrc == gelenPaketler[kanal].mevcut_crc);

    Serial.print(F("KANAL D"));
    Serial.print(22 + kanal);
    Serial.println(F(":"));

    Serial.print(F("  Paket alindi: "));
    Serial.println(paketAlindi[kanal] ? F("EVET") : F("HAYIR"));

    Serial.print(F("  Okunan byte sayisi: "));
    Serial.println(kanallar[kanal].okunanByteSayisi);

    Serial.print(F("  STX sayisi: "));
    Serial.println(kanallar[kanal].stxSayisi);

    Serial.print(F("  ETX sayisi: "));
    Serial.println(kanallar[kanal].etxSayisi);

    Serial.print(F("  Hata sayisi: "));
    Serial.println(kanallar[kanal].hataSayisi);

    Serial.print(F("  Parca numarasi: "));
    Serial.println(gelenPaketler[kanal].parca_numarasi);

    Serial.print(F("  Katman numarasi: "));
    Serial.println(gelenPaketler[kanal].katman_numarasi);

    Serial.print(F("  Veri uzunlugu: "));
    Serial.println(gelenPaketler[kanal].veri_uzunlugu);

    Serial.print(F("  Onceki CRC: "));
    uint32HexYazSerial(gelenPaketler[kanal].onceki_crc);
    Serial.println();

    Serial.print(F("  Mevcut CRC: "));
    uint32HexYazSerial(gelenPaketler[kanal].mevcut_crc);
    Serial.println();

    Serial.print(F("  Hesaplanan CRC: "));
    uint32HexYazSerial(hesaplananCrc);
    Serial.println();

    Serial.print(F("  CRC durumu: "));
    Serial.println(crcDogru ? F("DOGRU") : F("HATALI"));

    Serial.print(F("  Ilk 32 byte HEX: "));
    for (uint8_t i = 0; i < 32; i++) {
      byteHexYazSerial(gelenPaketler[kanal].yuk_verisi[i]);
      Serial.print(" ");
    }

    Serial.println();
    Serial.println();
  }

  Serial.println(F("BIRLESIK SIFRELI VERI HEX BASLANGIC"));
  Serial.println(F("--------------------------------------------------"));

  for (uint16_t i = 0; i < 2048; i++) {
    byteHexYazSerial(birlesikVeri[i]);
    Serial.print(" ");

    if ((i + 1) % 32 == 0) {
      Serial.println();
    }
  }

  Serial.println();
  Serial.println(F("--------------------------------------------------"));
  Serial.println(F("BIRLESIK SIFRELI VERI HEX BITIS"));
  Serial.println(F("=================================================="));
}

// ------------------------------------------------------
// SETUP
// ------------------------------------------------------
void setup() {
  Serial.begin(115200);

  lcd.begin(16, 2);
  lcd.clear();

  // D22-D25 giriş
  DDRA &= ~0x0F;

  // Pull-up aktif
  PORTA |= 0x0F;

  // D28 sanal TX cikis
  sanalTxHazirla();

  sistemiSifirla();

  lcdDurumYaz("Ara Mega Hazir", "4800 Baud RX");

  Serial.println(F("Ara Mega hazir."));
  Serial.print(F("ARA_MEGA_NO: "));
  Serial.println(ARA_MEGA_NO);
  Serial.print(F("UNO baud rate: "));
  Serial.println(UNO_BAUD_RATE);
  Serial.print(F("Bit suresi us: "));
  Serial.println(BIT_SURE_US);
  Serial.print(F("Ilk ornekleme us: "));
  Serial.println(ILK_ORNEKLEME_US);
  Serial.println(F("D22-D25 ayni anda sanal RX olarak dinleniyor."));
  Serial.println(F("D28 sanal TX olarak ayarlandi."));
}

// ------------------------------------------------------
// LOOP
// ------------------------------------------------------
void loop() {
  sistemiSifirla();

  lcdDurumYaz("4 Kanal RX", "4800 Baud");

  Serial.println();
  Serial.println(F("D22-D25 uzerinden 4800 baud 4 paket bekleniyor..."));

  uint32_t baslangic = millis();

  // Veri alma sirasinda LCD/Serial yazdirma yok
  while (!dortPaketTamamMi() && (millis() - baslangic < GENEL_TIMEOUT_MS)) {
    dortKanaliAyniAndaTara();
  }

  if (dortPaketTamamMi()) {
    paketleriBirlestir();

    lcdDurumYaz("4 Paket Alindi", "Birlestirildi");

    delay(500);

    lcdDurumYaz("Son Mega TX", "D28 Gonderiyor");

    // D28 uzerinden Son Mega'ya gonder
    sonMegayaCerceveGonder();

    lcdDurumYaz("Son Mega TX", "Gonderildi");

    delay(1000);

    // Ara Mega uzerinde de kontrol icin LCD'de goster
    lcdSifreliVeriGoster(5000);

    // PC Serial Monitor raporu
    raporBas();

    lcdDurumYaz("Rapor Bitti", "Tekrar bekle");

    delay(2000);
  } else {
    lcdDurumYaz("Timeout/Hata", "Serial bak");

    Serial.println();
    Serial.println(F("UYARI: 4 paket zamaninda tamamlanamadi."));

    for (uint8_t kanal = 0; kanal < 4; kanal++) {
      Serial.print(F("D"));
      Serial.print(22 + kanal);

      Serial.print(F(" | Paket="));
      Serial.print(paketAlindi[kanal] ? F("EVET") : F("HAYIR"));

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