from gpiozero import LED
from time import sleep
import threading

blue_led = LED(26)
white_led = LED(13)
motor_A = LED(17)
motor_B = LED(27)


def led_control():
	print("starting led thread")
	while True:
		blue_led.on()
		white_led.off()
		sleep(1)
		blue_led.off()
		white_led.on()
		sleep(1)

def motor_control():
	print("starting motor thread")
	while True:
		motor_A.on()
		motor_B.off()
		sleep(5)
		motor_A.off()
		motor_B.on()
		sleep(5)

def main():
	blue_led.on()
	white_led.on()
	motor_A.on()
	motor_B.on()

	threads = []
	led_t = threading.Thread(target=led_control)
	motor_t = threading.Thread(target=motor_control)
	threads.append(led_t)
	threads.append(motor_t)

	for t in threads:
		t.start()
	for t in threads:
		t.join()

if __name__ == '__main__':
	main()
