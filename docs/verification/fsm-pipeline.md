# FSM Pipeline Validation

## Boot Sequence


- Clean USB settle, threads suspended during jitter
- BOOT -> CALIBRATING -> ARMED in <1ms after settle

![alt text](image.png)


## Motion Detection Pipeline  

- MPU6050 motion at 2.46G (threshold 0.80G)
- Full ARMED -> EVENT_DETECTED -> SIGNING -> TRANSMITTING -> COOLDOWN
- End-to-end in ~336 microseconds

![alt text](image-1.png)

