#include <Arduino.h>
#include <SoftwareSerial.h>
#include <LiquidCrystal.h>

// --- PARAMETRIK AYARLAR ---
const uint8_t DUGUM_NO = 0;     // Her Uno icin 0-7 arasinda degistir
const uint8_t KATMAN_NO = 2;    // 2. Katman Uno
// ---------------------------------------------------------------

// LCD Ekran baglanti pinleri: (RS, EN, D4, D5, D6, D7)
LiquidCrystal lcd(8, 9, 10, 11, 12, 13);

// Donanimsal Serial portu
#define nisarial Serial

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

VeriPaketi alinanPaket;

// CRC32 Hata Kontrol Fonksiyonu
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

// 8 bitlik veriyi DUGUM_NO kadar saga dairesel kaydirir
uint8_t sagaDaireselKaydir(uint8_t veri, uint8_t kaydirma_miktari) {
  kaydirma_miktari = kaydirma_miktari % 8;

  if (kaydirma_miktari == 0) {
    return veri;
  }

  return (veri >> kaydirma_miktari) | (veri << (8 - kaydirma_miktari));
}

// Kriptografik Donusum Motoru
// 2. Katman Kurali:
// Alinan veri, DUGUM_NO kadar saga dairesel bit kaydirma islemiyle sifrelenir.
void guvenlikKatmaniIsle(VeriPaketi &paket) {
  // Guvenlik zinciri kurali: mevcut CRC eski CRC alanina yedeklenir
  paket.onceki_crc = paket.mevcut_crc;

  // Katman 2 sifreleme: her byte DUGUM_NO kadar saga dairesel kaydirilir
  for (uint16_t i = 0; i < 512; i++) {
    paket.yuk_verisi[i] = sagaDaireselKaydir(paket.yuk_verisi[i], DUGUM_NO);
  }

  // Paket artik 2. katmandan gecmis kabul edilir
  paket.katman_numarasi = KATMAN_NO;

  // Yeni sifreli verinin CRC degeri hesaplanir
  paket.mevcut_crc = crc32Hesapla(paket.yuk_verisi, 512);
}

void setup() {
  nisarial.begin(4800);

  lcd.begin(16, 2);
  lcd.clear();

  lcd.print("K");
  lcd.print(KATMAN_NO);
  lcd.print("-D");
  lcd.print(DUGUM_NO);
  lcd.print(" Beklemede");
}

void loop() {
  if (nisarial.available() > 0) {

    // STX kontrolu
    if (nisarial.read() == 0x02) {

      lcd.setCursor(0, 1);
      lcd.print("Mesaj Aliniyor..");

      byte* bellek_isaretci = (byte*)&alinanPaket;
      uint16_t okunan_bayt = 0;
      unsigned long zaman_asimi = millis();

      // Paket boyutu kadar veriyi oku
      while (okunan_bayt < sizeof(VeriPaketi) && (millis() - zaman_asimi < 1500)) {
        if (nisarial.available() > 0) {
          bellek_isaretci[okunan_bayt++] = nisarial.read();
        }
      }

      // ETX gelmesini bekle
      unsigned long etx_zaman_asimi = millis();
      while (nisarial.available() == 0 && (millis() - etx_zaman_asimi < 100)) {
        // Bekle
      }

      // Paket eksiksiz geldiyse ve ETX dogruysa
      if (okunan_bayt == sizeof(VeriPaketi) && nisarial.read() == 0x03) {

        // 2. katman sifreleme islemi
        guvenlikKatmaniIsle(alinanPaket);

        lcd.setCursor(0, 1);
        lcd.print("Mesaj Gonderildi");

        // Sifrelenmis paketi 3. katmana gonder
        nisarial.write(0x02); // STX
        nisarial.write((uint8_t*)&alinanPaket, sizeof(VeriPaketi));
        nisarial.write(0x03); // ETX

        delay(30);
      } else {
        lcd.setCursor(0, 1);
        lcd.print("Hata: Paket Eksik");
        delay(500);
      }

      lcd.setCursor(0, 1);
      lcd.print("                ");
    }
  }
}
