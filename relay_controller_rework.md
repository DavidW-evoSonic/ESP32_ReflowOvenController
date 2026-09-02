We're modifying this project to run with a hacked Sparkfun kit. The changes are:

Replace the MAX31855 SPI read with ADC1 read of the AD595 (10 mV/°C), oversampled, using esp_adc_cal
Code assumes phase-fired control with a zero-crossing detect input — you have a mechanical relay and no ZC circuit, so the output stage needs reworking to slow time-proportional or bang-bang
Profiles live in EEPROM, up to 30; profile select/edit needs surfacing to the web
WiFi provisioning is encoder-driven; needs replacing with hardcoded credentials or WiFiManager
IP discovery: serial print at boot or mDNS, since there's no display
Relay on a non-strapping GPIO (25 or 26); AD595 on ADC1 (32–39)
