cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(espaxa_zigbee)

target_sources(app PRIVATE
	src/main.c
	src/axa.c
	src/motion.c
	src/battery_level.c
)

target_sources_ifdef(CONFIG_ESPAXA_BATTERY app PRIVATE src/battery.c)
