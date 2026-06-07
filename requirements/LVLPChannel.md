# Versión 0 TFG Juan Estévez Delgado

# Requisitos generales

- El usuario debe de poder interactuar con el Tester través de una interfaz web, así como a través de un rotary encoder que se encuentra en el Tester. Para poder visualizar la información, lo podrá hacer tanto a través de la interfaz como a través de los leds RGB direccionables integrados en el Tester o la pantalla I2C que se encuentra en el Tester también
- El Tester debe de contar con canales I/O lo suficientemente versátiles para poder realizar todas las pruebas necesarias para poder comprobar el correcto funcionamiento de las PCBs (DUTs) que se encuentran en el monoplaza de la escudería del equipo de formula student MART (Málaga Racing Team)
- Debe de implementar medidas de seguridad hardware / software para proteger tanto a los DUTs como al Tester ante voltajes/corrientes excesivas
- Las comprobaciones a realizar por el Tester en los DUTs, deben de poder ser programadas por el usuario a través de las interfaces de una forma sencilla e intuitiva a través del firmware, mediante bloques, macros, HALs, previamente implementadas, así como conocer los resultados de dichas comprobaciones de una forma clara
- El tester debe de poder comunicarse con el exterior, a través de WiFi, CAN-BUS, SPI, I2C, UART
- La PCB del tester debe de ser diseñada de tal forma que proporcione aislamiento galvánico entre las zonas de alto y bajo voltaje por seguridad

# Arquitectura

**General**

El sistema completo consta de los siguientes bloques:

- La fuente de alimentación del sistema, comercial 15V dc con 3A de corriente máxima
- El sistema de pruebas (_tester_), una PCB que contará con todas las funcionalidades necesarias para comprobar el correcto funcionamiento del DUT
- La interfaz WEB, desde la cual el usuario podrá tanto diseñar y programar las pruebas que se realizarán de una forma intuitiva (sin usar código) como visualizar los datos con los resultados de las mismas. También será posible configurar el _tester_ desde aquí.
- El _Device Under Test (DUT)_ que será el sistema empotrado que se conectará al _tester_ a través del conector. Estos DUT serán las PCB del monoplaza de MART

Este TFG se centrará en el desarrollo del _tester_ y de la interfaz web

**Tester**

- Generación de señal: bloque en el que se genera la forma de la tensión de salida:
    - Tensión continua DC
    - Onda cuadrada (PWM)

- Amplificación: etapa que utiliza un operacional para poder generar y absorber más corriente
- Lectura: se lee la tensión y corriente del canal con un operacional y un ADC. Este último se comunica con el microcontrolador por SPI
- Protección y control:
    - Un relé de estado sólido (SSR) controlado por el microcontrolador se encarga de desconectar el conector I/O de la etapa de potencia tanto si se configura el canal como lectura como si se produce un consumo excesivo de corriente
    - Proteger de tensiones de más de 15V y de cortocircuitos

- Conector: sistema mecánico que proporciona la interfaz física con el medio (PCBs de prueba):
    - 8 canales I/O para señales de poca corriente (60mA máximo), el elemento activo que proporciona la tensión/corriente es el amplificador operacional
    - 2 canales para señales de más corriente (15A máximo). Controlados por un PMOS, a modo de interruptor. El microcontrolador controla el apagado y encendido de los PMOS y lee la tensión y la corriente que pasa por los mismos. No se regula la tensión como en los 8 canales I/O
    - 1 canal aislado para HV.

- Microcontrolador: encargado de controlar la tensión y la corriente tanto generada como absorbida de cada canal, realizar cálculos, controlar los sistemas de seguridad y comunicarse con el usuario a través de la interfaz gráfica

# Funcionalidad

Para poder verificar que el DUT funciona adecuadamente, el sistema de pruebas cuenta con una combinación de componentes y dispositivos que permiten que tenga las siguientes características:

- 3 tipos de canales:
    - 8 LPCH (Low Power Channel)
        - Lectura de tensión, corriente
        - Configurable como fuente de tensión, fuente de corriente, alta impedancia, carga resistiva variable
        - Generador de onda cuadrada PWM

- - 2 HPCH (High Power Channel)
        - Lectura de tensión, corriente
        - Sólo control de ON/OFF

- - 1 HVCH (High Voltage Channel)
        - Lectura de tensión
        - Sólo control de ON/OFF
        - Diodo de protección

Todos los canales tienen un fusible

- Protección (hardware y firmware), tanto para el _tester_ como para el DUT en los LPCH

- - Cada canal cuenta con un SSR (relé de estado sólido con protección térmica interna) controlado por el microcontrolador, que desconectará el puerto I/O del sistema en caso de detectar un consumo de corriente excesivo o una tensión que se salga del rango seguro, bien el establecido por el _datasheet_ de los componentes o por las características del DUT (se aplicará el más restrictivo).

Este será la primera medida de seguridad, gestionada por el firmware.

- - La combinación de un diodo TVS con un fusible protege al canal de tensiones y corrientes excesivas. Protección hardware pura, que actuará si la anterior falla

- El _tester_ será capaz de realizar pruebas para comprobar su correcto funcionamiento de forma automática.

- - Usar el conector con los puertos conectados entre sí (bien cortocircuitados o a través de resistencias de potencia): de esta manera sí que se podrán realizar pruebas en las que se pueda verificar que los canales tienen el comportamiento de generación/consumo de corriente que se espera de ellos.

- La interfaz WEB, desde la cual el usuario podrá tanto diseñar y programar las pruebas que se realizarán de una forma intuitiva (sin usar código) como visualizar los datos con los resultados de las mismas. También será posible configurar el _tester_ desde aquí.

# Bloque de alimentación (supply)

Se encarga de tomar los 15V que salen de la fuente de alimentación. Tanto la tensión como la corriente de la fuente se monitorean con el ADC MCP3208. La tensión la lee el ADC a través de un divisor resistivo y la corriente a través del ACS725 (conectado al MCP3208)

**Diferentes buses y sus funciones**

- Bus de 15V: alimenta a ICs(AO de los LVCH).
- Bus de 5V: alimentación del microcontrolador, ICs (AO del HVCH , MCP4728), LEDs
- Bus de 3.3V: alimenta a ICs (ACS725, AO aislamiento, registros de desplazamiento, MCP3208, Display)
- Bus de 15V aislado: alimenta al regulador de 5V aislado
- Bus de 5V aislado: alimenta a los ICs en la parte HV de la PCB, que controlan el HVCH

**Como se generan las tensiones**

- Bus de 15V: alimentado directamente a partir de la fuente
- Bus de 5V, 5V aislado y 3.3V a partir de reguladores lineales de tensión: LM7806 y AMS1117 respectivamente.
- Bis de 15V aislado con un DC-DC de aslamiento ( para generar tensiones con otra referencia de masa distinta a la de la fuente de 15V principal, ya que el HVCH está preparado para controlar una fuente de hasta 600V, de esta manera se proporciona seguridad

**Protecciones de los buses**

- Bus de 15V: protección contra polaridad inversa (Diodo), sobretensión (con TVS+Fusible) y sobrecorriente (fusible)

- Resto de buses: protección contra sobretensión (con TVS+Fusible) y sobrecorriente (fusible)

# Canales LPCH

Son 8 canales multifunción. (Canales n1 a n8)

- Lectura de tensión, corriente
- Configurable como fuente de tensión, fuente de corriente, alta impedancia, carga resistiva variable
- Generador de onda cuadrada PWM

Descripción circuital del canal:

GPAO de control

- Salida del DAC conectada al terminal no inversor del AO, a través de una resistencia de 1k. Esta resistencia de 1k coincide con el valor de la resistencia en paralelo que forman el lazo de realimentación, con el objetivo de minimizar el voltaje de offset a la salida del AO compensando las bias currents
- El lazo de realimentación esta formado por una resistencia de 3.9k (con un condensador de 1nF en paralelo para filtrar las altas frecuencias) y otra de 1k. Con esta configuración se consigue que se tenga una ganancia de casi 5.
- El AO se alimenta con 15V a través del bus de 15V, con un condensador de 100nF en la alimentación
- La red de realimentación se conecta a la salida del AO, antes de la resistencia shunt
- Cada integrado tiene 4 AO por lo que se pueden controlar 4 canales a la vez

La salida del AO va conectado a la resistencia shunt (de 10 ohmios), y el otro extremo va conectado a la entrada de un SSR, que tiene el papel de desconectar la salida del AO en caso de que se detecte un exceso de corriente o que el canal se configure en modo de alta impedancia. El led del SSR lo controla el ESP32, aunque no de forma directa a través de los GPIO, sino a través de un registro de desplazamiento.

La salida del SSR va conectado a un diodo TVS conectado a masa y a un fusible en serie con el conector I/O. El fusible y el diodo TVS se usan en conjunto para protejer de un exceso de corriente y de tensión

Cada canal usa 2 GPAO, uno de control (en configuración no inversora) para suministrar más tensión y corriente al canal a partir de los voltajes de entrada suministrados por el ESP32 y por el DAC y otro de lectura (configurado como seguidor de tensión, usado para no absorber corriente del canal y no afectar al voltaje para mantenerlo lo más realista posible

Lectura de tensión y corriente con los GPAO:

- Lectura de tensión: se usa el GPAO (TLV9364IPWR) para leer la tensión del bus para no afectar a la tensión (como pasaría si se usara un divisor resistivo directamente). El GPAO está configurado como un seguidor de tensión, en cuya salida hay un divisor resistivo que se encarga de reducir la tensión con un factor 6, para que esa tensión de salida esté siempre en el rango de (0-3.3V). La tensión generada por el divisor de tensión va a uno de los canales del ADC (MCP3208) que se comunica por SPI con el ESP32.

- Lectura de corriente: se usa una resistencia shunt en serie con la salida del GPAO que se encarga de controlar la tensión a la salida. La tensión antes de la resistencia se conoce (que es la tensión del DAC conectado al terminal negativo del GPAO multiplicada por la ganancia del AO). La tensión que hay después de esa resistencia también es conocida gracias al GPAO de lectura. Como el valor de la resistencia se conoce también, se calcula la corriente a partir de la ley de ohm

El GPAO de control, se puede configurar de dos maneras gracias a las tensiones de control:

- GPAOM1: Generación de señal PWM de amplitud variable: el ESP32 genera la señal PWM que entra a la patilla no inversora del GPAO, y a través del DAC se regula la amplitud máxima de la señal
- GPAOM2: Generación de tensión DC variable: el ESP32 pone su salida a 3.3V fijos y la tensión de salida del AO se regula a través del DAC

**Modos de funcionamiento:**

- Fuente de tensión / corriente regulable. GPAOM2. Se controla el DAC por software mediante el ESP32 a través de I2C para generar tensiones y corrientes variables. Al conocer tanto la tensión como la corriente a la salida por el GPAO de lectura, el microcontrolador usa esa información para controlar la tensión y corriente de salida. En el modo de fuente de tensión, con fijar la tensión del DAC será suficiente, y si se configura como fuente de corriente, se implementa un bucle de control el el ESP32 para variar la tensión del DAC para generar la corriente configurada a través de la shunt
- Carga resistiva variable: GPAOM2. Se regula la tensión de salida del AO, haciendo que esta esté por debajo de la tensión de entrada leída con el GPAO de lectura. Esto provoca que se genere una diferencia de tensión en la resistencia shunt, provocando que esta disipe calor y se comporte como una resistencia variable de potencia
- Modo alta impedancia: GPAOM2. Se abre el SSR de salida para desconectar el GPAO de control del conector ( y por tanto del DUT también) para no afectar a la tensión / corriente del canal. El GPAO de lectura se encarga de medir la tensión en el canal
- Generador de onda PWM: GPAOM1. Se genera una señal PWM de amplitud y frecuencia varibale.

En todos los modos, las funcionalidades de lectura de tensión y corriente están activas, salvo en el modo alta impedancia que la lectura de corriente no es posible (por la propia alta impedancia)

