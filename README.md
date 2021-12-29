# vidtex - videotex/viewdata client


> **See the installed man page for more detailed usage instructions**

**vidtex** is a Viewdata/Videotex client that can connect to services over TCP. Currently these services are NXTel, TeeFax, Telstar and Tetrachloromethane. Additional services may be configured by modifying the file \fIvidtexrc\fR. Alternatively you may supply the hostname and port number as options.


Viewdata uses a specific character set to transmit text and graphics characters. While many of these overlap with ASCII, most do not. Consequently, it is envisaged that the user will customise their terminal emulator to use a font that supports these characters.


By default the program outputs character codes compatible with the Bedstead font. However, the best experience will be had if a Galax Mode 7 font is used as this font supports double height characters. The Bedstead font was chosen as the default as it is most compatible with ASCII. For example, Galax causes '{' to be translated to '['.


The Bedstead font may be obtained from:


<https://bjh21.me.uk/bedstead/>


Several variations are available but the regular font is recommended:


<https://bjh21.me.uk/bedstead/bedstead.otf>


The Galax Mode 7 font may be obtained from:


<https://galax.xyz/TELETEXT/index.html>


Several variations are available but the following is recommended:


<https://galax.xyz/TELETEXT/MODE7GX3.TTF>

## Installation
1. wget ""
2. tar xvf vidtex-1.0.0.tar.gz
3. cd vidtex-1.0.0
4. ./configure
5. sudo make install

## Example Usage
    man vidtex
    #   If using the Galax mode 7 font
    vidtex --menu --galax
    #   If not
    vidtex --menu
