#include <Arduino.h>
#include <LiquidCrystal.h>

// LCD Ekran baglanti pinleri: (RS, EN, D4, D5, D6, D7)
LiquidCrystal lcd(7, 8, 9, 10, 11, 12);

// Arduino Mega 2560 icin:
// D22 -> PORTA bit 0
// D23 -> PORTA bit 1
// D24 -> PORTA bit 2
// D25 -> PORTA bit 3
// D26 -> PORTA bit 4
// D27 -> PORTA bit 5
// D28 -> PORTA bit 6
// D29 -> PORTA bit 7

// Kanal eslesmesi:
// Kanal 0 -> D22
// Kanal 1 -> D23
// Kanal 2 -> D24
// Kanal 3 -> D25
// Kanal 4 -> D26
// Kanal 5 -> D27
// Kanal 6 -> D28
// Kanal 7 -> D29

const uint16_t BAUD_HIZI = 4800;
const uint16_t BIT_SURE_US = 208; // 1 / 4800 saniye yaklasik 208 us

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

// 8 kanal icin paketler ayni anda hazirlanacak
VeriPaketi paketler[8];

// LCD kaydirma degiskenleri
int kaydirma_indeksi = 0;
const int kaydirma_hizi = 350;
unsigned long son_kaydirma_zamani = 0;

String kayan_yazi = "Mega Gonderiyor...   ";

// Serial Monitor'den gelen mesaj icin tampon
char bilgisayar_tamponu[513];
uint16_t tampon_uzunlugu = 0;

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

// LCD Ekran Yonetimi ve Kesintisiz Kaydirma Fonksiyonu
void lcdKayanYaziGuncelle() {
  if (kayan_yazi.length() == 0) return;

  if (millis() - son_kaydirma_zamani >= (unsigned long)kaydirma_hizi) {
    son_kaydirma_zamani = millis();
    lcd.setCursor(0, 0);

    String gosterilecek_metin = "";

    for (int i = 0; i < 16; i++) {
      int sonraki_karakter_indeksi = (kaydirma_indeksi + i) % kayan_yazi.length();
      gosterilecek_metin += kayan_yazi[sonraki_karakter_indeksi];
    }

    kaydirma_indeksi = (kaydirma_indeksi + 1) % kayan_yazi.length();
    lcd.print(gosterilecek_metin);
  }
}

// 8 kanala ayni anda 1 byte gonderir.
// Her kanal kendi byte'ini alir.
// Standart seri format: Start bit + 8 data bit + Stop bit
void paralelByteGonder(uint8_t kanal_byte[8]) {
  uint8_t port_degeri = 0;

  // START BIT: tum hatlar LOW
  PORTA = 0x00;
  delayMicroseconds(BIT_SURE_US);

  // 8 DATA BIT, LSB first
  for (uint8_t bit_no = 0; bit_no < 8; bit_no++) {
    port_degeri = 0;

    // Her kanal icin ilgili bit PORTA uzerindeki kendi pinine yazilir
    for (uint8_t kanal = 0; kanal < 8; kanal++) {
      if (kanal_byte[kanal] & (1 << bit_no)) {
        port_degeri |= (1 << kanal);
      }
    }

    PORTA = port_degeri;
    delayMicroseconds(BIT_SURE_US);
  }

  // STOP BIT: tum hatlar HIGH
  PORTA = 0xFF;
  delayMicroseconds(BIT_SURE_US);
}

// 8 paketi gercek anlamda ayni anda gonderir
void paketleriAyniAndaGonder(VeriPaketi paketler[8]) {
  uint8_t kanal_byte[8];

  lcd.setCursor(0, 1);
  lcd.print("Paralel gonder ");

  Serial.println("Paralel gonderim basladi.");

  // Gonderim sirasinda zamanlama bozulmasin diye interrupt kapatiliyor.
  // Bu kisimda Serial.print veya LCD islemi yapilmamali.
  noInterrupts();

  // STX: Baslama komutu
  for (uint8_t kanal = 0; kanal < 8; kanal++) {
    kanal_byte[kanal] = 0x02;
  }
  paralelByteGonder(kanal_byte);

  // Paket govdesi
  for (size_t byte_indeksi = 0; byte_indeksi < sizeof(VeriPaketi); byte_indeksi++) {
    for (uint8_t kanal = 0; kanal < 8; kanal++) {
      kanal_byte[kanal] = ((uint8_t*)&paketler[kanal])[byte_indeksi];
    }

    paralelByteGonder(kanal_byte);
  }

  // ETX: Bitis komutu
  for (uint8_t kanal = 0; kanal < 8; kanal++) {
    kanal_byte[kanal] = 0x03;
  }
  paralelByteGonder(kanal_byte);

  interrupts();

  Serial.println("Paralel gonderim tamamlandi.");

  lcd.setCursor(0, 1);
  lcd.print("Gonderildi      ");
  delay(300);

  lcd.setCursor(0, 1);
  lcd.print("                ");
}

// Girilen metni 8 parcaya boler ve paketleri hazirlar
void paketleriHazirla() {
  int toplam_uzunluk = tampon_uzunlugu;
  int temel_parca_boyutu = toplam_uzunluk / 8;
  int kalan_karakter = toplam_uzunluk % 8;
  int mevcut_okuma_indeksi = 0;

  unsigned long ortak_zaman_damgasi = millis();

  Serial.println();
  Serial.print("Gelen mesaj: ");
  Serial.println(bilgisayar_tamponu);

  Serial.print("Toplam uzunluk: ");
  Serial.println(toplam_uzunluk);

  Serial.println("Mesaj 8 parcaya bolunuyor...");

  for (uint8_t i = 0; i < 8; i++) {
    memset(&paketler[i], 0, sizeof(VeriPaketi));

    paketler[i].parca_numarasi = i;
    paketler[i].katman_numarasi = 1;
    paketler[i].zaman_damgasi = ortak_zaman_damgasi;
    paketler[i].onceki_crc = 0x00000000;

    int parca_boyutu = temel_parca_boyutu + (i < kalan_karakter ? 1 : 0);

    // Eski kod ile uyumlu kalsin diye 512 olarak birakildi.
    // Uno tarafinda mevcut sistem 512 byte yuk verisi bekliyorsa bunu degistirme.
    paketler[i].veri_uzunlugu = 512;

    if (parca_boyutu > 0 && mevcut_okuma_indeksi < toplam_uzunluk) {
      memcpy(
        paketler[i].yuk_verisi,
        &bilgisayar_tamponu[mevcut_okuma_indeksi],
        parca_boyutu
      );

      mevcut_okuma_indeksi += parca_boyutu;
    }

    paketler[i].mevcut_crc = crc32Hesapla(paketler[i].yuk_verisi, 512);

    Serial.print("Kanal ");
    Serial.print(i);
    Serial.print(" -> D");
    Serial.print(22 + i);
    Serial.print(" | Parca boyutu: ");
    Serial.println(parca_boyutu);
  }
}

void setup() {
  Serial.begin(115200);

  lcd.begin(16, 2);
  lcd.clear();

  lcd.setCursor(0, 0);
  lcd.print("Mega Hazir");
  lcd.setCursor(0, 1);
  lcd.print("D22-D29 paralel");

  // D22-D29 pinleri PORTA uzerindedir.
  // Tum PORTA cikis yapiliyor.
  DDRA = 0xFF;

  // Seri haberlesmede idle durum HIGH olmali.
  PORTA = 0xFF;

  memset(bilgisayar_tamponu, 0, sizeof(bilgisayar_tamponu));

  delay(1000);
  lcd.clear();

  Serial.println("Sistem hazir.");
  Serial.println("Paralel pin eslesmesi:");
  Serial.println("Kanal 0 -> D22");
  Serial.println("Kanal 1 -> D23");
  Serial.println("Kanal 2 -> D24");
  Serial.println("Kanal 3 -> D25");
  Serial.println("Kanal 4 -> D26");
  Serial.println("Kanal 5 -> D27");
  Serial.println("Kanal 6 -> D28");
  Serial.println("Kanal 7 -> D29");
  Serial.println();
  Serial.println("Serial Monitor'e metin yazip Enter'a basin.");
}

void loop() {
  lcdKayanYaziGuncelle();

  while (Serial.available() > 0) {
    char gelen_karakter = Serial.read();

    if (gelen_karakter == '\n' || gelen_karakter == '\r') {
      if (tampon_uzunlugu > 0) {
        bilgisayar_tamponu[tampon_uzunlugu] = '\0';

        kayan_yazi = String(bilgisayar_tamponu) + "   ";
        kaydirma_indeksi = 0;
        lcd.clear();

        paketleriHazirla();

        // Burada artik 8 Uno'ya sirayla degil, ayni anda gonderiliyor.
        paketleriAyniAndaGonder(paketler);

        // Tampon temizleniyor
        tampon_uzunlugu = 0;
        memset(bilgisayar_tamponu, 0, sizeof(bilgisayar_tamponu));
      }
    } else {
      if (tampon_uzunlugu < 512) {
        bilgisayar_tamponu[tampon_uzunlugu] = gelen_karakter;
        tampon_uzunlugu++;
      }
    }
  }
}