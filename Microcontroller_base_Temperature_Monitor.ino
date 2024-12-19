#define F_CPU 16000000UL
#include <avr/io.h>
#include <util/delay.h>
#include <avr/interrupt.h>

// Define LED pins
#define LED1 PD5
#define LED2 PD6
#define LED3 PB1
#define LED4 PB2
#define LED5 PB3
#define LED6 PD7

// Define button pins
#define BUTTON_INC PD4
#define BUTTON_DEC PD3
#define BUTTON_RESET PD2  // Reset button
#define BUTTON_MODE PD1   // Mode button
#define BUTTON_SSD PD0    // SSD on/off and brightness button

// Define segment pins (74HC595)
#define DS PC1    // Data pin for 74HC595
#define SHCP PC3  // Shift Clock pin for 74HC595
#define STCP PC2  // Storage Clock pin for 74HC595

// Define digit control pins
#define DIGIT1 PB0
#define DIGIT2 PB5
#define DIGIT3 PC4
#define DIGIT4 PB4

volatile uint8_t value = 0;
volatile uint8_t current_digit = 0;
volatile uint8_t reset_flag = 0;
volatile uint8_t mode = 0;      // 0 = Celsius, 1 = Fahrenheit
volatile uint8_t ssd_state = 1; // SSD on/off state (1 = on, 0 = off)
volatile uint8_t ssd_cycle_state = 0; // Cycle state for PD0 button
volatile uint8_t led_brightness_state = 1; // LED brightness state

// Array to hold segment configurations for digits 0-9
const uint8_t segment_map[] = {
	0b00111111,  // 0
	0b00000110,  // 1
	0b01011011,  // 2
	0b01001111,  // 3
	0b01100110,  // 4
	0b01101101,  // 5
	0b01111101,  // 6
	0b00000111,  // 7
	0b01111111,  // 8
	0b01101111   // 9
};

// Segment configurations for characters 'C' and 'F'
const uint8_t segment_C = 0b00111001; // Segments a, d, e, f
const uint8_t segment_F = 0b01110001; // Segments a, b, e, f, g

void shift_out(uint8_t data);
void display_digit(uint8_t digit);
void display_character(uint8_t character);

void setup() {
	// Set LED pins as output
	DDRD |= (1 << LED1) | (1 << LED2) | (1 << LED6);
	DDRB |= (1 << LED3) | (1 << LED4) | (1 << LED5) | (1 << DIGIT1) | (1 << DIGIT2) | (1 << DIGIT4);
	DDRC |= (1 << DIGIT3) | (1 << DS) | (1 << SHCP) | (1 << STCP);

	// Set push button pins as input with pull-up resistors
	DDRD &= ~(1 << BUTTON_INC) & ~(1 << BUTTON_DEC) & ~(1 << BUTTON_RESET) & ~(1 << BUTTON_MODE) & ~(1 << BUTTON_SSD);
	PORTD |= (1 << BUTTON_INC) | (1 << BUTTON_DEC) | (1 << BUTTON_RESET) | (1 << BUTTON_MODE) | (1 << BUTTON_SSD);

	// Enable pin change interrupts for PD4, PD3, PD2, PD1, and PD0
	PCICR |= (1 << PCIE2); // Enable Pin Change Interrupt for PCINT23..16 (PD0..PD7)
	PCMSK2 |= (1 << PCINT20) | (1 << PCINT19) | (1 << PCINT18) | (1 << PCINT17) | (1 << PCINT16); // Enable interrupt for PD4, PD3, PD2, PD1, and PD0

	// Configure Timer1 for display multiplexing
	TCCR1A = 0x00;
	TCCR1B = (1 << WGM12) | (1 << CS11); // CTC mode, prescaler 8
	OCR1A = 1999;                        // Interrupt every 1ms
	TIMSK1 = (1 << OCIE1A);              // Enable Timer1 compare interrupt

	// Configure Timer0 for Fast PWM on PD5 (OC0B)
	TCCR0A = (1 << WGM00) | (1 << WGM01) | (1 << COM0B1); // Fast PWM mode, clear OC0B on compare match
	TCCR0B = (1 << CS01);                                 // Prescaler 8
	OCR0B = 0;                                            // Initial duty cycle

	// Enable global interrupts
	sei();
}

ISR(PCINT2_vect) {
	// Check if PD4 is pressed (Increment button)
	if (!(PIND & (1 << BUTTON_INC))) {
		if (value < 49) {
			value++;
			if (value <= 15 && led_brightness_state == 1) {
				OCR0B = value * 17; // Increase brightness proportionally (255 / 15 = 17)
			}
		}
		_delay_ms(5);// Reduced debounce delay
	}
	// Check if PD3 is pressed (Decrement button)
	if (!(PIND & (1 << BUTTON_DEC))) {
		if (value > 0) {
			value--;
			if (value <= 15 && led_brightness_state == 1) {
				OCR0B = value * 17; // Decrease brightness proportionally
			}
		}
		_delay_ms(5);// Reduced debounce delay
	}
	// Check if PD2 is pressed (Reset LED6 button)
	if (!(PIND & (1 << BUTTON_RESET))) {
		reset_flag = 1;
		PORTD &= ~(1 << LED6);  // Turn off LED6
		_delay_ms(5);// Reduced debounce delay
	}
	// Check if PD1 is pressed (Mode button)
	if (!(PIND & (1 << BUTTON_MODE))) {
		mode = !mode;  // Toggle mode
		_delay_ms(5); // Reduced debounce delay
	}
	// Check if PD0 is pressed (SSD on/off and brightness button)
	if (!(PIND & (1 << BUTTON_SSD))) {
		ssd_cycle_state = (ssd_cycle_state + 1) % 3; // Cycle through 0, 1, 2

		switch (ssd_cycle_state) {
			case 0:
			ssd_state = 0; // Turn off SSD
			break;
			case 1:
			ssd_state = 1; // Turn on SSD
			led_brightness_state = 0; // Set LED brightness to zero
			break;
			case 2:
			ssd_state = 1; // Turn on SSD
			led_brightness_state = 1; // Restore original LED brightness
			break;
		}
		_delay_ms(5);//Reduced debounce delay
	}
}

ISR(TIMER1_COMPA_vect) {
	if (ssd_state == 0) {
		// Turn off all segments when SSD is off
		shift_out(0);
		PORTB &= ~((1 << DIGIT1) | (1 << DIGIT2) | (1 << DIGIT4));
		PORTC &= ~(1 << DIGIT3);
		return;
	}

	uint8_t display_value = value;
	if (mode == 1) {
		// Convert to Fahrenheit
		display_value = (value * 9 / 5) + 32;
	}

	uint8_t hundreds = (display_value / 100) % 10;
	uint8_t tens = (display_value / 10) % 10;
	uint8_t units = display_value % 10;

	// Turn off all digit control pins
	PORTB &= ~((1 << DIGIT1) | (1 << DIGIT2) | (1 << DIGIT4));
	PORTC &= ~(1 << DIGIT3);

	// Set segments based on the current digit
	switch (current_digit) {
		case 0:
		display_digit(hundreds);
		PORTB |= (1 << DIGIT1);
		break;
		case 1:
		display_digit(tens);
		PORTB |= (1 << DIGIT2);
		break;
		case 2:
		display_digit(units);
		PORTC |= (1 << DIGIT3);
		break;
		case 3:
		if (mode == 0) {
			display_character(segment_C); // Display 'C' for Celsius
			} else {
			display_character(segment_F); // Display 'F' for Fahrenheit
		}
		PORTB |= (1 << DIGIT4);
		break;
	}

	current_digit++;
	if (current_digit > 3) {
		current_digit = 0;
	}
}

void shift_out(uint8_t data) {
	// Shift out 8 bits of data to the 74HC595 shift register
	for (uint8_t i = 0; i < 8; i++) {
		// Set data pin according to the current bit
		if (data & (1 << (7 - i))) {
			PORTC |= (1 << DS); // Set data pin high
			} else {
			PORTC &= ~(1 << DS); // Set data pin low
		}

		// Pulse the shift clock
		PORTC |= (1 << SHCP);   // Set shift clock high
		_delay_us(1);           // Small delay for stabilization
		PORTC &= ~(1 << SHCP);  // Set shift clock low
		_delay_us(1);
	}

	// Pulse the storage clock to latch the data
	PORTC |= (1 << STCP);   // Set storage clock high
	_delay_us(1);
	PORTC &= ~(1 << STCP);  // Set storage clock low
}

void display_digit(uint8_t digit) {
	uint8_t segments = segment_map[digit];
	shift_out(segments); // Send segment data to the shift register
}

void display_character(uint8_t character) {
	shift_out(character); // Send character data to the shift register
}

void loop() {
	// Turn off all LEDs except PB3 (LED5 blinking handled separately)
	PORTD &= ~((1 << LED1) | (1 << LED2));
	PORTB &= ~((1 << LED3) | (1 << LED4));

	// Light the appropriate LEDs based on the value
	if (value <= 15 && led_brightness_state == 1) {
		PORTD |= (1 << LED1);
		// The brightness of LED1 (PD5) is handled by PWM in the ISR
		if (value == 0 || value == 1) {
			// Ensure LED1 is on for very low values
		}
		} else if (value <= 25 && led_brightness_state == 1) {
		PORTD |= (1 << LED2); // Turn on LED2
		OCR0B = 0; // Ensure PWM is turned off for LED1
		} else if (value <= 35 && led_brightness_state == 1) {
		PORTB |= (1 << LED3); // Turn on LED3
		OCR0B = 0; // Ensure PWM is turned off for LED1
		} else if (value <= 40 && led_brightness_state == 1) {
		PORTB |= (1 << LED4); // Turn on LED4
		OCR0B = 0; // Ensure PWM is turned off for LED1
		} else if (value > 40) {
		if (!reset_flag) {
			PORTD |= (1 << LED6); // Turn on LED6
		}
		// Blink LED5 (PB3)
		PORTB |= (1 << LED5);
		_delay_ms(500);
		PORTB &= ~(1 << LED5);
		_delay_ms(500);
		return; // Skip the rest of the loop to maintain the blink
	}

	// Reset flag handling
	if (reset_flag) {
		// Reset flag handling when the value is 40 or below
		if (value <= 40) {
			reset_flag = 0; // Clear the reset flag
		}
	}

	_delay_ms(70);// Small delay to reduce flicker
}

int main() {
	setup();
	while (1) {
		loop();
	}
}