64tass --nostart ultimax_init.a
xcopy /Y a.out ultimax_init
bin2c -o ultimax_init.h ultimax_init

64tass --nostart ultimax_memcfg.a
xcopy /Y a.out ultimax_memcfg
bin2c -o ultimax_memcfg.h ultimax_memcfg
