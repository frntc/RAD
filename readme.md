<img  align="right"  width="280"  src="https://raw.githubusercontent.com/frntc/RAD/main/Images/rad_logo.jpg">
  

The **RAD Expansion Unit** is a cartridge/expansion for the C64 and C128 using a Raspberry Pi 3A+, 3B+ or Zero 2 to implement the actual functionality. It emulates a *RAM Expansion Unit* up to 16mb (compatible to CBM 1700/1750/1764 REU, CLD Super 1750 Clone, CMD 1750/1750XL) and a *GeoRAM/NeoRAM memory expansion* up to 4mb. It also features a menu to browse, manage and launch REU- and GeoRAM-images, NUVIEs, PRGs and Vice Snapshots (VSF). 

The RAD also supports an external disk drive and printer emulator, the [**IECBuddy**](https://github.com/dhansel/IECBuddy/), which is the result of a great cooperation with David Hansel. The IECBuddy can be used to synchronize files with the RAD and is integrated into its menu. 

The RAD is designed to not only emulate existing extensions, other things that have already been tested (but not yet included here) are, for example, MOS 6510/8500 emulation (incl. turbo mode), using the RAD as a (co-)processor (in fact the menu runs on the ARM CPU only). With the RAD you can also play [Doom on your C64/C128](https://github.com/frntc/RAD-Doom).

  
RAD's functionality is entirely defined by software. The connecting circuitry is quite simple and does not include any programmable ICs and is thus easy to build. It's a sibling of my other projects: [Sidekick64](https://github.com/frntc/Sidekick64), [SIDKick pico](https://github.com/frntc/SIDKick-pico) and [SIDKick](https://github.com/frntc/SIDKick).

The RAD has been tested with various PAL-machines (C64s, C128, C128D) and NTSC-C64s/C128s, as well as with the Ultimate64 (the inner workings of the Commodore 64 Ultimate). More in-depth tests with SX64 or C64 Reloaded boards remain to be carried out (not by me, I don't own such machines/devices, but the RAD was reported to work).
  

<p  align="center"  font-size:  30px;>

  

<img  src="https://raw.githubusercontent.com/frntc/RAD/main/Images/rad_menu.jpg"  height="200">

<img  src="https://raw.githubusercontent.com/frntc/RAD/main/Images/rad_cartridge.jpg"  height="200">

<img  src="https://raw.githubusercontent.com/frntc/RAD/main/Images/rad_case_bigby.jpg"  height="200">

  

</p>

  

## Using the RAD

Assuming you have your hardware ready and set up the SD card (see below for instructions), the RAD will boot the C64/C128 into its main menu unless configured to not do so. In the menu you can browse and select files:

### Main menu 
| key | command |
|----------|:-------------|
| H | display help |
| cursor keys, F1/F3, <br> HOME / DEL | navigate files / directories <br> go to first entry, directory up |
| RETURN | start PRG or select image, press 2x to autostart NUVIE or GeoRAM images |
| S | mark or unmark a file for syncing (transfer to and from) the IECBuddy (*) |
| U / N | unmount the image of the memory expansion, or name & save it to SD |
| D / R | delete or rename a file on the SD card |
| I | go to IECBuddy submenu (*) |
| K | launch SIDKick (pico) configuration (only if detected) |
| £ | timings configuration submenu | 

(*) (only if IECBuddy detected)

### IECBuddy submenu


<img  align="right"  src="https://github.com/dhansel/IECBuddy/blob/main/images/IECBuddy-Mini1.jpg"  height="150">

<img  align="right"  src="https://raw.githubusercontent.com/frntc/RAD/main/Images/iecbuddy_transfer.jpg"  height="150">

The IECBuddy submenu is only accessible if an IECBuddy (image shows one of the variants) has been connected to the RAD before booting. You can synchronize files on the SD card with the internal flash storage of the IECBuddy, and newly created files on the IECBuddy are transfered to the subdirectory 'IECBuddy' in the RAD's 'PRG' folder.

| IECBuddy menu | command |
|----------|:-------------|
| T | transfer/sync files from and to IECBuddy <br> (pending transfers are shown at the bottom) (**) |
| 1..0, <br> Q | select favorite disk-images for quick swap (button on the IECBuddy) <br> remove from favorite list, note: imagescan also be mounted using  <br> DOS commands (as with SD2IEC) |
| W | wipe all: **remove all** synced files from IECBuddy |
| I | initialize the IECBuddy (= 'factory reset'), **removes all files** |
| + / - | change drive number of the IECBuddy |
| ← | back to main menu | 

(**) if you transfer disk images (D64, D71, etc.) you can provide a .gif-image (240x160 resolution) which is displayed on the IECBuddy-TFT-screen once the image is mounted.

### Printer emulation with the IECBuddy

<img  align="right"  src="https://raw.githubusercontent.com/frntc/RAD/main/Images/iecbuddy_print.jpg"  height="150">

The IECBuddy emulates a printer (device #4). When you print on the C64/C128 and go back to the RAD menu you can preview and save the print by pressing 'I' (go to IECBuddy submenu) in the main menu. 

| print submenu | command |
|----------|:-------------|
| F7 | see a preview of the print |
| RETURN | save the print (type in name before). <br> The print will be save as PDF and series of BMPs <br> on the SD-card in the sub-folder 'RAD_PRINT'. |
| ← | back to main menu | 

In the print preview you can scroll and zoom through the entire (multipage) document:

| print preview | command |
|----------|:-------------|
| cursor keys, or WASD | scroll over printout |
| + / - | zoom |
| SPACE | reset scaling |
| F1 / F3 | go one page up / down |
| F2 / F4 | go to first / last page |
| HOME / DEL | go to begin / end of current page |
| F7 or ← | close preview |



## How to build a RAD Expansion Unit:

  

<img  align="right"  src="https://raw.githubusercontent.com/frntc/RAD/main/Images/rad_render.jpg"  height="200">

  

This sections summarize building and setting up the hardware.
  

### PCBs, BOM and assembly information

  

There are two variants of the PCB: a larger one that fits the dimensions of the Raspberry Pi 3A+/3B+ and a smaller one which is tailored for the Raspberry Pi Zero 2. All is interchangeable, i.e. you can use either PCB with any of the supported RPis.

  

Here you can find the BOM and assembly information for [RAD v0.1](https://htmlpreview.github.io/?https://github.com/frntc/RAD/blob/master/Gerber/ibom.html); both PCBs have exactly the same location of components, except for the buttons. Sourcing components: you can use LVC and LVX types for the 245, 257, and 573 ICs, you can use a 74LS30 or 74HCT30, make sure that you get the correct CBTD3861. The tactile switches, socket and ICs cost about 4 to 6 EUR/USD/CAD from reknown vendors such as Mouser, Digikey, TME if you buy components to 10 RADs (add about 20% if you only buy for one). Including the PCB the costs easily stay below 7 to 9 EUR/USD/CAD. (everything without shipping.)

### PCB ordering

If you want to support my projects, feel free to order the PCBs from PCBWay:
- [RAD for RPi 3A+/3B+](https://www.pcbway.com/project/shareproject/RAD_Expansion_Unit_C64_C128_Cartridge_8dc5e00e.html)
- [RAD for RPi Zero 2](https://www.pcbway.com/project/shareproject/RAD_Expansion_Unit_C64_C128_Cartridge_small_version_de0002a3.html)
- If you don't have an account at PCBWay yet: [register via this link](https://pcbway.com/g/x1UjP0) and get "$5 of New User Free Credit".

The Gerber files for PCB-production are also available in this repository if you want to order from another PCB manufacturer.
  

### Build instructions

The first step when building the RAD is soldering the surface-mount components (bottom side only). Next solder the socket for the Raspberry Pi (2x20 female pin header), the pin header and push buttons. That's it!

  

### 3D printed cases

bigby has designed a case for 3D printing for both the RPi 3A+ RAD-PCB (see image above, courtesy of bigby) and the Zero2 PCB. The files can be downloaded on [Thingiverse](https://www.thingiverse.com/thing:5733086) and [Printables](https://www.printables.com/model/345964-rad-expansion-unit-case).
As the PCBs have the some format as the Sidekick64 PCBs, other cases might be easy to adapt.

  

## Quick start

Simply copy the release files  onto an SD card (FAT32 formatted). You might want to have a look at the **configuration file** SD:RAD/rad.cfg, where you can setup the preferred startup mode (menu, as REU, or as GeoRAM). Also copy your REU/GeoRAM-images and .PRGs to the respective subdirectories.

  

From the menu you can select/browse using the keyboard. Press 'H' for help.

  

If you want to change the menu music, you can replace SD:RAD/music.wav. The format is a standard .WAV-file (preferred: mono, 15.6kHz, 8-bit PCM) of max. 8mb.

  

## Powering the RAD Expansion Unit

Please use an external power supply for the RPi. Although the circuitry has pull-ups/pull-downs to not mess with the bus at boot time, the safest way is to boot the RAD first and then turn on the C64/C128.

  

Alternatively you can power the RAD from the **C64/C128** using the "close to power..."-jumper. The current of the RPi has been measured and stays within the specifications of the expansion port, but (for some still unknown reason) it requires a quite strong and very stable power supply and often does not work at all. If it does and you experience any instabilities when powering from the expansion port (e.g. crashes during NUVIE-playing) it's most likely because of that. This is why I strongly recommend to use a good external power supply.

  

**Important:**  **NEVER** power the RAD from the computer and externally at the same time. NEVER!

  

## Overclocking

The Raspberry Pi configuration (SD:config.txt) overclocks the RPi (moderately for the RPi 3A+/B+, pretty significant for the Zero 2). Although I never experienced any problems with it, please be aware that overclocking may void warranty. In case you feel that RAD is not running stable, feel free to experiment with the values (some hints given in the configuration file), and let me know which settings work best for your configuration!

## Configuration Details, Trouble Shooting

The bus timings and cache parameters are stored in SD:RAD/rad.cfg -- in most cases there is no need to modify these values... unless you notify glitches (e.g. when playing NUVIEs or BluREU). I experienced such with the (only) cartridge port expander (I own). In the configuration file there are alternative timings which remove these problems on my machines. It might happen that similar issues occur with other expanders or machines with other expansion port setups (SX64, which I can't test). The same counter measures should help there. Also one tester reported problems with his ASSY 250407 C64. Adjusting the timings WAIT_ENABLE_RW_ADDRLATCH and WAIT_ENABLE_DATA_WRITING (e.g. in +/- 10 steps) helped. If you experience problems, reach out for me on forum64.de.

  
## Vice Snapshots

Vice snapshots are stored as VSF-files and can be created using the Vice emulator (use latest release!). They are similar to what freezer cartridges do: storing the current state of the computer. The RAD will restore the memory, VIC, SID, CIA, and CPU states from the stored VSF data (however, timers and CPU-VIC-sync are not restored absolutely accurate).

Keep in mind that snapshots have to be used with care: they work for C64s and C128s in C64-mode, but a C64-VSF might not work on a C128 and vice versa. Also, for example, if you snapshot a C64 in Vice and then load the VSF on a C64 with a different kernal ROM the machine might crash (ROMs are not replaced in the real machine).


## Known limitations/bugs

Please keep in mind that you're not reading about a product, but my personal playground that I'm sharing. Not everything might work without glitches, in particular given the early development stage. 

One known glitch is the sideborder scroller in Treulove which is not rendered correctly (contact me if you know what the reason might be).
  

## Building the code (if you want to)

Set up your Circle44.3 and gcc-arm environment, then you can compile RAD almost like any other example program (the repository contains the build settings for Circle that I use -- make sure you use them, otherwise it will probably not work correctly). The C64/C128 code is compiled using 64tass.

  

## Disclaimer

Be careful not to damage your RPi or 8-bit computer, or anything attached to it. I am not responsible if you or your hardware gets damaged. In principle, you can attach the RPi and other cartridges at the same time, as long as they do not conflict (e.g. in IO ranges or driving signals). I recommend to NOT do this. Again, I'm not taking any responsibility -- if you don't know what you're doing, better don't... use everything at your own risk.

  
## Where did you get your RAD? How to get one?

You've built it yourself? Cool, this project is for tinkerers!

If you have questions about assembling one, don't hesitate to ask!

If you can't build one yourself, you can get pre-assembled PCBs from PCBWay or from official sellers of Sidekick64/RAD/SIDKick, e.g. ausvantage_online, stmlord and jamessahm on ebay (more to come).

*It is also perfectly fine and appreciated* if someone sells spare PCBs (populated or not) of a PCB-order or manufactures a small batch and offers them on a forum, but I expect the price tag to be lower than that of the aforementioned official sellers.

If you bought a Sidekick64/RAD/SIDKick for the same price as from the official sellers or even more, it's likely that someone does this for money and violates the license. It's annoying when open source/CC developers' licenses are not respected and that's why I'm starting to welcome these sellers to the RAD-Hall of Shame:
<table>
<tr>
<td>  <img  height="150"  src="https://raw.githubusercontent.com/frntc/RAD/main/Images/rad_hos_1.jpg">  </td>
<td>A very active HoS-member offering not just one, but three of my projects :-(
</tr>
<td>  <img  height="150"  src="https://raw.githubusercontent.com/frntc/SIDKick-pico/main/Images/hos_6.jpg">  </td>
<td>... and another one ...
</tr>
</table>

  
## License

The *source code* is licensed under *GPLv3*.

The *PCB* is work licensed under a *Creative Commons Attribution-NonCommercial-ShareAlike 4.0 International License*.

The font of the RAD logo and cartridge labels build on/have been derived from Elemental End (https://www.dafont.com/elementalend.font) which is free for non-commercial use under the CC BY-NC-ND 3.0 license.
  

## Misc

Last but not least I would like to thank a few people. This project is based on my experience with Sidekick64, please have a look for the acknowledgements there as well. In particular I'd like to thank here: my testers (emulaThor, bigby, and TurboMicha), androSID for valuable advice with ICs, Rene Stange (the author of Circle) who is always responsive and helping with special requests. Thanks also go to Groepaz for helping with insights about the C64, and the rest of the Vice team for sharing their emulator which is a great reference for emulating hardware. The default soundtrack in the menu is *Molecule's Revenge* by Jester (https://modarchive.org/module.php?52333), the menu uses Retrofan's system font (https://compidiaries.wordpress.com/)

  

### Trademarks

Raspberry Pi is a trademark of the Raspberry Pi Foundation.
