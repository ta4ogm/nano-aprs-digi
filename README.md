# Nano APRS Digi

Arduino Nano tabanlı, bilgisayar gerektirmeyen APRS digipeater (WIDEn-N / fill-in) ve konum beacon'ı.

**Ayar aracı:** https://ta4ogm.github.io/nano-aprs-digi/

Çağrı işaretinizi, konumunuzu, açıklamayı, beacon aralığını ve digi tipini girin, size özel `.hex` dosyasını indirin. Derleme gerekmez; araç hazır yazılımın içindeki ayar bloğunu tarayıcıda değiştirir.

Daha önce bu araçla oluşturduğunuz bir `.hex` dosyasını **"Mevcut bir hex dosyasını düzenle"** bölümünden yükleyip ayarlarını değiştirebilirsiniz. (Ayar bloğu olmayan eski sürümler düzenlenemez.)

## Özellikler

- WIDEn-N digi (WIDE2-2'ye kadar, ayarlanabilir) veya fill-in (sadece WIDE1-1)
- Kendi paketini tekrarlamaz, aynı paketi iki kez göndermez
- Sabit konum beacon'ı (aralık, ilk beacon süresi, yol, simge, açıklama ayarlanabilir)
- Serial Monitor (9600) çıktısı: `Paket geldi: KAYNAK>HEDEF,YOL: bilgi` ve tekrarlanıp tekrarlanmadığı
- Serial Monitor'de `b` + Enter: anında beacon, `t` + Enter: anında telemetri (test için)
- Telemetri (isteğe bağlı, 30/60 dk): saatlik duyulan paket (Rx1h) ve saatlik tekrarlanan paket (Dg1h). Sayılar olduğu gibi gönderilir (en fazla 255/saat).
- Açıklamaya yükseklik eklenebilir (`/A=` alanı, aprs.fi'de "alt … m")
- İsteğe bağlı PHG (güç, anten yüksekliği, kazanç, yön); aprs.fi bundan kapsama mesafesini hesaplar

## Donanım

BeRTOS / MicroModem tipi devre:

| Nano pini | Görev |
|---|---|
| A0 | Telsizden gelen ses (10K/10K bias, 100 nF kuplaj) |
| D4–D7 | R-2R ses DAC'ı (8K2, 3K9, 2K2, 1K) → pot → telsize giden ses |
| D3 | PTT (1K → BC547) |
| D9 / D10 | TX / RX LED |

### Kenwood tipi (Baofeng, TYT…) telsiz bağlantısı

| Fiş | Bacak | Bağlantı |
|---|---|---|
| 2.5 mm | uç | telsizden gelen ses |
| 2.5 mm | gövde | GND |
| 3.5 mm | orta halka | telsize giden ses, araya **1 µF** (+ bacak telsiz tarafında) |
| 3.5 mm | gövde | PTT (transistörün kollektörü) |
| 3.5 mm | uç | **boş** (telsizden voltaj çıkar) |

Kondansatör olmazsa ya da ters takılırsa telsiz sürekli yayında kalır. Büyük değerler (47 µF gibi) fişler takılınca telsizi uzun süre yayına sokar.

## Yükleme

1. Serial Monitor açıksa kapatın.
2. XLoader → hex dosyası → **Duemilanove/Nano (ATmega328)**, 57600. Hata verirse **Uno** (115200).
3. Upload. Kontrol için Serial Monitor'ü 9600 hızında açın.

## Kaynaktan derleme

```sh
sudo apt install gcc-avr avr-libc binutils-avr
./build.sh
```

`build.sh` yazılımı derler, `firmware/digi_base.hex` dosyasını ve ayar aracının sayfası olan `index.html`'i günceller. Varsayılan ayarlar `firmware/main.c` içindeki `CFG` bloğundadır.

## Lisans

Bu proje Mark Qvist'in [MicroDigi](https://github.com/markqvist/MicroDigi) projesi (BeRTOS tabanlı) temel alınarak geliştirilmiştir ve **GPL-3.0** lisanslıdır. Ayrıntılar için `LICENSE` dosyasına bakın.

Çağrı işaretiyle yayın yapmak için geçerli bir amatör telsiz lisansı gerekir.
