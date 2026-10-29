# path ~/.config/GnuChanScreenShoot/GnuChanScreenShoot.py
#
# WARNING: bu gercek bir Python betigi degil, diger Gnuchan programlarinin
# ayar dosyalariyla ayni tadda bir AYAR dosyasidir. GnuChanScreenShoot bunu
# okuyup davranisini secer.
#
# Ekran gorusunu nerede kaydedecegini, hangi formatta, hangi tuslarla
# alacagini burada ayarlarsin. Degistirdigin degeri kaydet, bir sonraki
# calismada gecerli olur.

Version = "0.2.0"

gcl_Screenshot.Settings(
    # Ekran goruslerinin kaydedilecegi dizin. ~ ev dizinine expand edilir.
    # Varsayilan ~/Pictures.
    SaveDir="~/Pictures",

    # Kayit formati. "png" (kayipsiz) veya "jpeg".
    # Varsayilan png.
    Format="png",

    # JPEG kalitesi, 1 ile 100 arasindaki bir sayi. PNG icin yok sayilir.
    # Varsayilan 90.
    Quality=90,

    # Alt+P'nin odakli pencereyi mi (True) yoksa tum ekrani mi (False)
    # yakalayacagini soyler. Varsayilan True.
    CaptureFocused=True,

    # Print/SysRq tusuna basildiginda ayar penceresinin acilip acilmayacagini
    # soyler. Varsayilan True.
    SettingsWindow=True,
)
