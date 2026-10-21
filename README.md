# light-sensor-labview

PCF8591 LDR 수집 및 TCP 전송 프로그램

Raspberry Pi에서 `/dev/i2c-1`을 직접 사용해 PCF8591의 AIN0~AIN3 값을 읽는다.
내장 LDR을 손으로 가리고 떼면서 가장 크게 변하는 채널을 찾는 첫 번째 하드웨어 시험용 프로그램이다.

- `pcf8591_reader`: AIN0~AIN3을 모두 출력하는 배선·채널 확인 프로그램
- `light_sensor_server`: 선택한 채널을 읽어 TCP 클라이언트에 JSON으로 보내는 서버
- `tcp_test_client.py`: LabVIEW를 만들기 전에 PC에서 통신을 확인하는 시험 클라이언트

## 사전 확인

```bash
sudo i2cdetect -y 1
```

표에 `48`이 보여야 한다.

## 빌드

```bash
sudo apt update
sudo apt install -y build-essential i2c-tools
make
```

## 센서 채널 확인

```bash
./pcf8591_reader
```

권한 오류가 발생할 때만 다음처럼 시험한다.

```bash
sudo ./pcf8591_reader
```

출력은 `time_s,AIN0,AIN1,AIN2,AIN3` 순서이며 ADC 원시값의 범위는 0~255다.
LDR을 가렸을 때 값이 커지는지 작아지는지는 보드 회로에 따라 다르다.
가리고 떼는 동작에 가장 분명하게 반응하는 채널을 LDR 채널로 선택한다.
종료는 `Ctrl+C`를 누른다.

## TCP 서버 실행

가리고 떼었을 때 가장 크게 변했던 채널 번호를 첫 번째 인자로 지정한다.
아래 예시는 `AIN0`, TCP 포트 `5000`을 사용한다.

```bash
./light_sensor_server 0 5000
```

I2C 권한 오류가 발생할 때만 다음처럼 시험한다.

```bash
sudo ./light_sensor_server 0 5000
```

서버가 실행되면 다음과 같이 표시된다.

```text
Light sensor TCP server ready: port=5000 channel=AIN0
Waiting for one PC client. Press Ctrl+C to stop.
```

첫 버전은 PC 클라이언트 한 대만 받는다. 클라이언트가 끊어지면 측정을 중지하고 다음 연결을 기다린다.

## Windows PC에서 통신 시험

Raspberry Pi와 같은 네트워크에 연결된 Windows PowerShell에서 실행한다.
`PI_IP`는 실제 Raspberry Pi 주소로 바꾼다.

```powershell
cd "C:\Users\ys991\OneDrive\문서\ChatGPT\New project\light_sensor_pi"
python .\tcp_test_client.py PI_IP --port 5000 --seconds 5
```

Python 실행 명령이 `py`인 환경에서는 다음을 사용한다.

```powershell
py .\tcp_test_client.py PI_IP --port 5000 --seconds 5
```

시험 클라이언트는 상태 조회, 측정 시작, 5초간 데이터 수신, 측정 중지를 차례로 수행한다.
센서를 가리고 떼면서 `adc_raw` 값이 변하는지 확인한다.

```json
{"type":"response","request_id":2,"result":"ok","state":"running"}
{"type":"sample","seq":0,"adc_raw":182,"status":"ok"}
{"type":"sample","seq":1,"adc_raw":179,"status":"ok"}
{"type":"response","request_id":3,"result":"ok","state":"stopped"}
```

## TCP 메시지 규칙

모든 JSON 메시지 끝에는 줄바꿈(`\n`)을 붙인다. TCP에서 한 메시지가 나뉘거나 여러 메시지가 함께 도착해도 서버는 완성된 한 줄씩 처리한다.

지원 명령은 다음 세 가지다.

```json
{"type":"command","request_id":1,"cmd":"start"}
{"type":"command","request_id":2,"cmd":"stop"}
{"type":"command","request_id":3,"cmd":"status"}
```

측정 중에는 약 100ms마다 ADC 원시값을 보낸다. `seq`는 새 측정을 시작할 때 0부터 시작한다.

```json
{"type":"sample","seq":0,"adc_raw":182,"status":"ok"}
```

잘못된 명령이나 메시지는 장치를 제어하지 않고 오류 응답을 보낸다. ADC 읽기에 실패하면 `0`을 정상값처럼 보내지 않고 센서 읽기 오류로 보고한다.
