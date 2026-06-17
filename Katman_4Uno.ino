#include <Arduino.h>
#include <LiquidCrystal.h>

// --- PARAMETRIK AYARLAR ---
const uint8_t DUGUM_NO = 7;     // Her Uno icin 0-7 arasinda degistir
const uint8_t KATMAN_NO = 4;    // 4. Katman Uno

// Dugum 0-3 -> 1. Ara Mega
// Dugum 4-7 -> 2. Ara Mega
const uint8_t ARA_MEGA_NO = (DUGUM_NO <= 3) ? 1 : 2;

// Ara Mega tarafindaki sanal RX girisi
// Dugum 0 veya 4 -> Ara Mega D22
// Dugum 1 veya 5 -> Ara Mega D23
// Dugum 2 veya 6 -> Ara Mega D24
// Dugum 3 veya 7 -> Ara Mega D25
const uint8_t ARA_MEGA_SANAL_RX_PINI = 22 + (DUGUM_NO % 4);
// ---------------------------------------------------------------

// LCD Ekran baglanti pinleri: (RS, EN, D4, D5, D6, D7)
LiquidCrystal lcd(8, 9, 10, 11, 12, 13);

// Uno donanimsal seri portu
// RX0: 3. katmandan gelen veri
// TX1: Ara Mega D22-D25 sanal RX hattina giden veri
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

// 4. Katman bayt permutasyonu icin hedef indeks uretir.
// 512 = 2^9 oldugu icin carpim katsayisi tek secildi.
// Bu sayede 0-511 araliginda cakismasiz yer degistirme yapilir.
uint16_t permutasyonIndeksiUret(uint16_t mevcut_indeks) {
  uint16_t carpim_katsayisi = (uint16_t)((2 * DUGUM_NO) + 1);
  uint16_t kaydirma_katsayisi = (uint16_t)((KATMAN_NO * 53) + (DUGUM_NO * 37));

  return (uint16_t)((mevcut_indeks * carpim_katsayisi + kaydirma_katsayisi) % 512);
}

// Kriptografik Donusum Motoru
// 4. Katman Kurali:
// Alinan veri bayt permutasyonu yani yer degistirme islemiyle sifrelenir.
// Ardindan Uno TX1 pininden Ara Mega D22-D25 sanal RX hattina gonderilir.
void guvenlikKatmaniIsle(VeriPaketi &paket) {
  // Guvenlik zinciri kurali: mevcut CRC eski CRC alanina yedeklenir
  paket.onceki_crc = paket.mevcut_crc;

  uint8_t gecici_yuk[512];
  memset(gecici_yuk, 0, sizeof(gecici_yuk));

  // Bayt permutasyonu
  for (uint16_t i = 0; i < 512; i++) {
    uint16_t yeni_indeks = permutasyonIndeksiUret(i);
    gecici_yuk[yeni_indeks] = paket.yuk_verisi[i];
  }

  memcpy(paket.yuk_verisi, gecici_yuk, 512);

  // Paket artik 4. katmandan gecmis kabul edilir
  paket.katman_numarasi = KATMAN_NO;

  // Yeni sifreli verinin CRC degeri hesaplanir
  paket.mevcut_crc = crc32Hesapla(paket.yuk_verisi, 512);
}

void setup() {
  nisarial.begin(4800);

  lcd.begin(16, 2);
  lcd.clear();

  lcd.setCursor(0, 0);
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

        // 4. katman bayt permutasyonu
        guvenlikKatmaniIsle(alinanPaket);

        lcd.setCursor(0, 1);
        lcd.print("M");
        lcd.print(ARA_MEGA_NO);
        lcd.print(" D");
        lcd.print(ARA_MEGA_SANAL_RX_PINI);
        lcd.print(" Gonder");

        // Sifrelenmis paketi Ara Mega'ya gonder
        // STX ve ETX aynen korunuyor
        nisarial.write(0x02); // STX
        nisarial.write((uint8_t*)&alinanPaket, sizeof(VeriPaketi));
        nisarial.write(0x03); // ETX

        delay(30);

        lcd.setCursor(0, 1);
        lcd.print("Gonderildi      ");
        delay(300);
      } else {
        lcd.setCursor(0, 1);
        lcd.print("Hata: Paket Eksik");
        delay(500);
      }

      lcd.setCursor(0, 1);
      lcd.print("                ");

      lcd.setCursor(0, 1);
      lcd.print("M");
      lcd.print(ARA_MEGA_NO);
      lcd.print(" RX:D");
      lcd.print(ARA_MEGA_SANAL_RX_PINI);
    }
  }
}

