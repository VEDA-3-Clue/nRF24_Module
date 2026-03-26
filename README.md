# nRF24_Module

nRF24L01 기반의 저대역폭 무선 링크 위에 TUN 인터페이스를 올려  
IP 패킷을 전송하는 실험용 네트워크 모듈입니다.

이 프로젝트는 Raspberry Pi + nRF24L01 조합을 사용하여  
간단한 point-to-point IP 링크를 구성하는 것을 목표로 합니다.

기존 구현은 `Primary / Secondary` 기반의 polling 구조였으나,  
현재 버전은 이를 확장하여 다음과 같은 **Coordinator + Peer + PENDING/GRANT/DATA/ACK**  
프레임 체계를 사용합니다.

즉, 제어 역할은 Coordinator가 담당하되,  
데이터 전송은 기존의 단순 polling보다 더 효율적인 **burst 기반 교환 구조**로 동작합니다.

---

## 1. 주요 특징

- nRF24L01 기반 무선 링크
- Raspberry Pi TUN 인터페이스 기반 IP 통신
- 단일 RF 링크 위에 IP packet forwarding
- 기존 polling 병목을 줄이기 위한 MAC frame 기반 프로토콜
- `PENDING / GRANT / DATA / ACK / RESET` 프레임 지원
- burst 기반 fragment 전송
- TUN을 통한 `ping`, `iperf`, `UDP`, `RTP` 등 실험 가능

---

## 2. 시스템 개요

프로젝트는 두 개의 노드로 구성됩니다.

- **Coordinator**
- **Peer**

논리적으로는 Coordinator가 세션 및 교환 흐름을 주도하지만,  
데이터 자체는 어느 쪽에서든 큐에 쌓일 수 있으며,  
Peer 역시 `PENDING`을 통해 자신에게 전송할 데이터가 있음을 광고할 수 있습니다.

### 기존 구조와 차이점

기존 구조:
- Primary가 Secondary를 지속적으로 poll
- Secondary는 요청이 올 때까지 대기
- 빈 poll이 많고 uplink 지연이 큼

현재 구조:
- Coordinator가 기본 keepalive/poll을 수행
- Peer는 `PENDING`으로 대기 중인 데이터 존재를 광고
- Coordinator는 `GRANT`를 통해 burst 전송 기회를 부여
- 실제 데이터는 `DATA` 프레임으로 연속 전송
- `ACK`는 누적 ACK(cumulative ACK) 방식으로 처리

---

## 3. 전체 아키텍처

```text
+-------------------+                       +-------------------+
|   Coordinator     | <---- nRF24L01 ----> |       Peer        |
|-------------------|                       |-------------------|
| TUN interface     |                       | TUN interface     |
| MAC scheduler     |                       | MAC responder     |
| Pending/Grant     |                       | Pending advertise |
| Fragment TX/RX    |                       | Fragment TX/RX    |
+-------------------+                       +-------------------+
```

## Docker 를 사용하여 개발환경 구축 하기

1. image 만들기<br/>
    app-dev 이미지가 없다면 아래 명령을 통해 도커 이미지를 구축할 수 있습니다.<br/>
    이미 이미지를 생성 되어있다면 중복해서 생성하실 필요가 없습니다.<br/>
    `docker build -t app-dev -f tools/Docker-env/docker-app/Dockerfile .`

## container 만들기<br/>
    컨테이너는 아래 명령을 통해 바로 작업 폴더에 마운트를하여 사용할 수 있습니다.<br/>
    `docker run --rm -it --name rf-dev-container -v "${PWD}:/workspace" -w /workspace app-dev bash`


## 프로젝트 빌드 방법

`cmake --preset aarch64-release`
`cmake --build --preset aarch64-release`
`cpack --preset aarch64-release`

## 실행 방법

Coordinator / Peer 양쪽 모두 동일한 RF 설정을 사용해야 합니다.
특히 `--channel`, `--primary_addr`, `--secondary_addr`, 그리고 RF 튜닝 옵션은 반드시 양 끝단이 일치해야 합니다.

### Coordinator 실행 예시

```bash
./build/aarch64-release/nerfnet/nerfnet \
  --primary \
  --interface_name nerf0 \
  --tunnel_ip 192.168.10.1 \
  --tunnel_mask 255.255.255.0 \
  --channel 1 \
  --poll_interval_us 100
```

### Peer 실행 예시

```bash
./build/aarch64-release/nerfnet/nerfnet \
  --secondary \
  --interface_name nerf0 \
  --tunnel_ip 192.168.10.2 \
  --tunnel_mask 255.255.255.0 \
  --channel 1
```

### run.sh 실행 예시

패키징 후에는 아래처럼 `run.sh` 를 사용하는 편이 편합니다.
기본 RF 설정으로 실험할 때는 `--rf_*` 옵션을 주지 않으면 됩니다.

```bash
sudo ./run.sh --primary --ce_pin 25
sudo ./run.sh --secondary --ce_pin 25
```

### RX IRQ 옵션

선택적으로 nRF24L01 의 `IRQ` 핀을 보드 GPIO 에 연결한 뒤 `--irq_pin` 으로 지정할 수 있습니다.
이 옵션을 주면 RX 대기 구간에서 GPIO interrupt 를 사용하고, 주지 않으면 기존 polling 방식으로 동작합니다.

`--irq_pin` 은 이제 GPIO line offset 기준 사용을 권장합니다.
기존 sysfs global GPIO 번호도 호환 차원에서 계속 받을 수 있습니다.

- 권장: GPIO line offset 예 `6`
- 호환: sysfs global GPIO 번호 예 `518`

예를 들어 Raspberry Pi 에서 physical pin 31 = GPIO 6 인 경우 아래처럼 실행할 수 있습니다.

```bash
sudo ./run.sh --primary --ce_pin 25 --irq_pin 6
sudo ./run.sh --secondary --ce_pin 25 --irq_pin 6
```

기존처럼 sysfs global GPIO 번호를 직접 넣어도 됩니다.

```bash
sudo ./run.sh --primary --ce_pin 25 --irq_pin 518
sudo ./run.sh --secondary --ce_pin 25 --irq_pin 518
```

주의:
- Coordinator 와 Peer 모두 같은 방식으로 실행하는 것을 권장합니다.
- 현재 IRQ 구현은 선택적 실험 기능이며, GPIO IRQ 설정에 실패하면 자동으로 polling 으로 폴백합니다.
- 보드/커널에서 `/sys/class/gpio` 를 지원해야 합니다.

### RF 튜닝 옵션

다음 옵션으로 nRF24 링크 파라미터를 실행 시점에 조정할 수 있습니다.

- `--rf_data_rate {250kbps|1mbps|2mbps}`
- `--rf_crc {8|16}`
- `--rf_retry_delay {0..15}`
- `--rf_retry_count {0..15}`

`--rf_retry_delay` 는 RF24 기준 1 step = 250us 이며, 실제 auto-retry delay는 250us ~ 4000us 범위입니다.

실측 기준 현재는 RF 기본 설정을 그대로 사용하는 쪽이 더 좋았습니다.

- `--rf_data_rate 2mbps`
- `--rf_crc 8`
- `--rf_retry_delay 0`
- `--rf_retry_count 15`

즉 아래처럼 `--rf_*` 옵션을 생략한 기본 실행이 현재 기준선입니다.

```bash
sudo ./run.sh --primary --ce_pin 25
sudo ./run.sh --secondary --ce_pin 25
```

참고:
- `1mbps + CRC16 + retry_delay 6 + retry_count 12` 도 실험했지만, 현재 보드/환경에서는 기본값보다 지연과 손실이 더 커졌습니다.
- 따라서 RF 튜닝은 현재는 기본값 유지 상태에서 비교 실험용 옵션으로 보는 것이 맞습니다.

## 현재 실측 결과

아래 수치는 현재 기본 RF 설정(`2mbps + CRC8 + retry_delay 0 + retry_count 15`)과
`kPostDataExchangeGapUs = 250` pacing 패치 기준에서 측정한 결과입니다.

### Ping

- 기본 ping: `0% loss`, 평균 RTT 약 `198 ms`
- `ping -s 32`: `0% loss`, 평균 RTT 약 `146 ms`
- `ping -s 128`: `0% loss`, 평균 RTT 약 `262 ms`
- `ping -s 256`: 약 `5% loss`, 평균 RTT 약 `313 ms`

해석:
- 작은 payload는 안정적입니다.
- `128B`도 현재는 안정적으로 수신됩니다.
- `256B`는 사용 가능하지만 지연과 재전송 비용이 여전히 존재합니다.

### TCP 간단 검증

- `nc` 기반 echo 테스트에서 `4 KiB` 수준의 데이터 왕복 확인
- 작은 TCP interactive / 지속 전송은 가능
- 다만 더 큰 지속 전송에서는 여전히 지연 누적 가능성 있음

### UDP 실측

`iperf3 -u` 기준 측정:

- `64B @ 10k`: sender `0% loss`
- `128B @ 10k`: sender `0% loss`
- `256B @ 10k`: receiver 약 `1% loss`, jitter 약 `55 ms`
- `256B @ 15k`: receiver `0% loss`, jitter 약 `140 ms`, receiver completion time 약 `39.7 s`
- `256B @ 20k`: receiver 약 `1% loss`, jitter 약 `179 ms`, receiver completion time 약 `54.7 s`

해석:
- 현재 링크는 `256B`에서도 낮은 loss로 버틸 수 있습니다.
- 하지만 bitrate를 올리면 손실보다 먼저 queue buildup 과 latency inflation 이 커집니다.
- 현재 기준으로 `256B payload`의 안정 운용점은 대략 `10k` 부근으로 보는 것이 합리적입니다.

### 현재 단계 평가

- 링크 안정화와 MAC 신뢰성 확보는 상당 부분 달성됨
- 작은 UDP/TCP는 안정화 단계에 진입
- 현재 남은 핵심 과제는 대역폭 확대보다는 지연 감소와 처리율 개선
- 다음 우선순위는 RF 재시도 미세조정, failure handling 보강, IRQ 검토 순서가 적절함
