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

단일 RF 기준 주요 실행 옵션:

- `--ce_pin`: nRF24L01 CE 핀
- `--csn_pin`: nRF24L01 SPI CSN 핀
- `--channel`: RF 채널
- `--irq_pin`: 선택적 RX IRQ GPIO

듀얼 RF 사용 시 추가 옵션:

- `--radio2_ce_pin`: 두 번째 nRF24L01 CE 핀
- `--radio2_csn_pin`: 두 번째 nRF24L01 SPI CSN 핀
- `--radio2_channel`: 두 번째 RF 채널, 생략 시 `channel + 1`
- `--radio2_irq_pin`: 두 번째 nRF24L01 IRQ GPIO

### Coordinator 실행 예시

```bash
./build/aarch64-release/nerfnet/nerfnet \
  --primary \
  --interface_name nerf0 \
  --tunnel_ip 192.168.10.1 \
  --tunnel_mask 255.255.255.0 \
  --ce_pin 25 \
  --csn_pin 0 \
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
  --ce_pin 25 \
  --csn_pin 0 \
  --channel 1
```

### run.sh 실행 예시

패키징 후에는 아래처럼 `run.sh` 를 사용하는 편이 편합니다.
기본 RF 설정으로 실험할 때는 `--rf_*` 옵션을 주지 않으면 됩니다.

```bash
sudo ./run.sh --primary --ce_pin 25 --csn_pin 0
sudo ./run.sh --secondary --ce_pin 25 --csn_pin 0
```

### Dual radio 병렬 모드

2개의 nRF24L01 을 병렬로 사용할 때는 각 모듈이 서로 다른 `CE`, `CSN`, `channel`
조합을 가져야 합니다. 주소(`primary_addr`, `secondary_addr`)는 그대로 공유해도 되며,
채널만 분리해서 같은 TUN 인터페이스에 병렬로 붙습니다.

현재 구현 방식:

- `radio0`, `radio1` 각각이 독립적인 기존 MAC 세션으로 동작합니다.
- 두 세션은 같은 TUN 인터페이스를 공유하며 병렬로 패킷을 운반합니다.
- 즉, 2채널 병렬 대역폭 확장용 1차 구조이며 링크 간 재정렬 가능성은 남아 있습니다.

기본 동작:

- `radio0`: `--ce_pin`, `--csn_pin`, `--channel`, `--irq_pin`
- `radio1`: `--radio2_ce_pin`, `--radio2_csn_pin`, `--radio2_channel`, `--radio2_irq_pin`
- `--radio2_channel` 을 생략하면 기본값은 `channel + 1`
- `--radio2_ce_pin` 을 주지 않으면 기존 단일 RF 모드로 동작합니다.

Coordinator 예시:

```bash
sudo ./run.sh \
  --primary \
  --ce_pin 25 --csn_pin 0 --channel 1 --irq_pin 6 \
  --radio2_ce_pin 24 --radio2_csn_pin 1 --radio2_channel 2 --radio2_irq_pin 5
```

Peer 예시:

```bash
sudo ./run.sh \
  --secondary \
  --ce_pin 25 --csn_pin 0 --channel 1 --irq_pin 6 \
  --radio2_ce_pin 24 --radio2_csn_pin 1 --radio2_channel 2 --radio2_irq_pin 5
```

직접 바이너리 실행 예시:

```bash
./build/aarch64-release/nerfnet/nerfnet \
  --primary \
  --interface_name nerf0 \
  --tunnel_ip 192.168.10.1 \
  --tunnel_mask 255.255.255.0 \
  --ce_pin 25 --csn_pin 0 --channel 1 --irq_pin 6 \
  --radio2_ce_pin 24 --radio2_csn_pin 1 --radio2_channel 2 --radio2_irq_pin 5 \
  --poll_interval_us 100
```

주의:

- `radio0` 와 `radio1` 의 `CE`, `CSN` 은 반드시 서로 달라야 합니다.
- `radio0` 와 `radio1` 의 `channel` 도 서로 다르게 쓰는 것을 권장합니다.
- 양 끝단은 동일한 dual-radio 배치로 실행해야 합니다.
- 한쪽만 dual-radio 로 실행하면 기대한 병렬 효과를 얻을 수 없습니다.
- 패킷은 두 링크로 분산되어 흐르므로 장기적인 성능 튜닝은 `send_fail`, 채널 간 편차, 재정렬 영향까지 함께 봐야 합니다.

### Dual radio 현재 판단 메모

기록 시각: `2026-03-30 02:35:50 UTC`

최근 dual-radio 실험 결과 기준 현재 단계의 판단은 아래와 같습니다.

- dual-radio 는 초기의 심한 재정렬/중복 문제는 완화되었지만, 아직 single-radio 최적 조합보다 확실한 우위를 만들지는 못했습니다.
- 특히 `iperf3 -u -l 256 -b 15k -t 20` 기준으로 receiver loss/jitter 는 개선되었지만, 여전히 `send_fail`, `rx_timeout`, 세션 간 비대칭이 남아 있습니다.
- 따라서 현 시점의 dual-radio 는 "이상적인 2채널 bonding" 이라기보다, 실험적 병렬 운반 구조에 가깝습니다.

현재 단계의 한계:

- 두 radio 가 완전히 통합된 하나의 전송 계층처럼 동작하지는 않습니다.
- 링크별 지연 차이, 재전송, ACK 흐름이 남아 있어 장시간 부하에서 성능 편차가 큽니다.
- 양방향 통신은 가능하지만, 영상 uplink 와 제어 downlink 를 동시에 안정적으로 최적화한 상태는 아직 아닙니다.
- 따라서 "2채널이면 single 보다 항상 좋아야 한다" 는 기대를 현재 구현은 만족하지 못합니다.

대안 1:

- 실사용 기준선은 당분간 single-radio 최적 조합으로 유지합니다.
- 즉 `2mbps + CRC8 + retry_delay 0 + retry_count 15`, `kPostDataExchangeGapUs = 250`, `libgpiod IRQ` 조합을 기본 권장 경로로 둡니다.
- RC 차량 초기 통합 단계에서는 먼저 single 기준으로 영상/제어 양방향을 안정화하는 것이 가장 안전합니다.

대안 2:

- dual-radio 는 "병렬 분산" 대신 "역할 분리" 방향으로 발전시킵니다.
- 예를 들어 `radio0` 는 bulk uplink(카메라/센서), `radio1` 는 control/ACK/downlink 우선으로 두는 구조가 현실적입니다.
- 이 방향은 이상적인 bonding 은 아니지만, RC 차량처럼 `영상 uplink + 제어 downlink` 가 공존하는 시스템에서는 single 보다 나은 체감 성능을 만들 가능성이 더 큽니다.

### RX IRQ 옵션

선택적으로 nRF24L01 의 `IRQ` 핀을 보드 GPIO 에 연결한 뒤 `--irq_pin` 으로 지정할 수 있습니다.
이 옵션을 주면 RX 대기 구간에서 GPIO interrupt 를 사용하고, 주지 않으면 기존 polling 방식으로 동작합니다.

`--irq_pin` 은 이제 GPIO line offset 기준 사용을 권장합니다.
기존 sysfs global GPIO 번호도 호환 차원에서 계속 받을 수 있습니다.

- 권장: GPIO line offset 예 `6`
- 호환: sysfs global GPIO 번호 예 `518`

예를 들어 Raspberry Pi 에서 physical pin 31 = GPIO 6 인 경우 아래처럼 실행할 수 있습니다.

```bash
sudo ./run.sh --primary --ce_pin 25 --csn_pin 0 --irq_pin 6
sudo ./run.sh --secondary --ce_pin 25 --csn_pin 0 --irq_pin 6
```

기존처럼 sysfs global GPIO 번호를 직접 넣어도 됩니다.

```bash
sudo ./run.sh --primary --ce_pin 25 --csn_pin 0 --irq_pin 518
sudo ./run.sh --secondary --ce_pin 25 --csn_pin 0 --irq_pin 518
```

주의:
- Coordinator 와 Peer 모두 같은 방식으로 실행하는 것을 권장합니다.
- 현재 IRQ 구현은 선택적 실험 기능이며, GPIO IRQ 설정에 실패하면 자동으로 polling 으로 폴백합니다.
- 현재 구현은 `libgpiod` 기반이며, line offset 방식 사용을 권장합니다.

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

## 현재 기준선

현재 코드는 아래 조합을 기준선으로 유지합니다. 이 기준선은 가장 좋았던 실측 결과를 보였던 축을 기준으로 정리한 것이며, 최근의 미세 실험 중 성능 개선이 입증되지 않은 것은 채택하지 않았습니다.

### 기준선에 남긴 것

- RF 설정: 기본값 유지 (`2mbps + CRC8 + retry_delay 0 + retry_count 15`)
- MAC 기본값: `kDefaultBurstGrant = 3`
- 안정화 패치: duplicate DATA 허용, invalid reassembled packet drop, TUN read sanity check
- CPU/타이밍 개선: busy-wait 완화, `kPostDataExchangeGapUs = 250`
- IRQ: `libgpiod` 기반 RX IRQ (`--irq_pin 6` 같은 GPIO line offset 권장)
- fairness 1차: peer `pending=1`일 때 coordinator local DATA burst를 2개로 제한 후 `Grant` 우선
- TX failure recovery: `flush_tx + short gap`만 유지, 강제 `startListening()` recovery는 사용하지 않음
- 계측 유지: frame type별 `send_fail` 카운터 (`sf_pend/sf_grant/sf_data/sf_ack/sf_reset`)

### 기준선에서 채택하지 않은 것

- RF 튜닝 실험: `1mbps + CRC16 + retry_delay 6 + retry_count 12` 등은 기본값보다 나빴음
- 더 긴 pacing: `500us`는 `250us`보다 나빴음
- TX recovery 에서 강제 `startListening()` 추가: 성능 회귀 발생
- `Grant/Ack` 전용 turnaround gap: 개선 불충분, 일부 구간에서 오히려 악화
- 최근 실험 중 성능 향상이 재현되지 않은 미세 timing tweak 들은 현재 baseline 에 포함하지 않음

### 기준선 실행 예시

```bash
sudo ./run.sh --primary --ce_pin 25 --irq_pin 6
sudo ./run.sh --secondary --ce_pin 25 --irq_pin 6
```

IRQ 없이 비교할 때는 아래처럼 실행합니다.

```bash
sudo ./run.sh --primary --ce_pin 25
sudo ./run.sh --secondary --ce_pin 25
```

## 현재 실측 결과

아래 표와 수치는 지금까지의 대표 실험 결과를 단계/버전별로 정리한 것입니다.
현재 가장 안정적으로 보이는 조합은 기본 RF 설정(`2mbps + CRC8 + retry_delay 0 + retry_count 15`),
`kPostDataExchangeGapUs = 250` pacing, `libgpiod` 기반 RX IRQ(`--irq_pin 6`)입니다.

### 단계별 요약

| 단계 | 주요 변경 | 대표 결과 | 해석 |
| --- | --- | --- | --- |
| 초기 안정화 이전 | duplicate DATA 미처리, reassembly 검증 부족 | 큰 payload 에서 링크 붕괴 잦음 | 구조 자체보다 recovery/validation 부족이 문제였음 |
| 공통 안정화 적용 | duplicate DATA 허용, invalid reassembled packet drop, busy-wait 완화 | 기본 ping/`32B` 안정화 | 링크가 복구 가능한 느린 저대역 상태로 올라옴 |
| 기본 RF + grant 3 정착 | `kDefaultBurstGrant = 3` 유지 | `128B`까지 실사용 가능 수준 | MAC 기본 방향은 유지 가능 |
| pacing 적용 | `kPostDataExchangeGapUs = 250` | `ping -s 256`에서 `0% loss`, avg 약 `328 ms` | 큰 payload 안정성 개선 |
| IRQ 정착 | `libgpiod` 기반 RX IRQ + pacing 유지 | `ping -s 256`에서 `0% loss`, avg 약 `331 ms` | IRQ는 CPU/수신 대기 개선용으로 유효, 핵심 병목은 여전히 `send_fail` |

### 버전별 비교

| 버전/조합 | 실행 조건 | `ping -s 256` 결과 | 비고 |
| --- | --- | --- | --- |
| 기본 RF, 초기 안정화 직후 | polling | 약 `71% loss`, avg 약 `840 ms` | 큰 payload 는 아직 붕괴 구간 |
| 기본 RF, 안정화 후 | polling | 약 `5% loss`, avg 약 `313 ms` | `256B`가 사용 가능한 단계로 올라옴 |
| 기본 RF + pacing `250us` | polling | `0% loss`, avg 약 `328 ms` | 현재 채택 후보의 출발점 |
| 기본 RF + pacing `250us` + sysfs IRQ 시도 실패 | polling fallback | `0% loss`, avg 약 `312 ms` | 실제 IRQ 효과는 아님 |
| 기본 RF + pacing `250us` + `libgpiod` IRQ | `--irq_pin 6` | `0% loss`, avg 약 `331 ms` | 현재 기준선 |
| `1mbps + CRC16 + retry_delay 6 + retry_count 12` | RF 튜닝 실험 | 약 `22% loss`, avg 약 `782 ms` | 기본값보다 나쁨 |
| `retry_delay 8`, `retry_count 12` | RF 튜닝 실험 | 약 `19% loss`, avg 약 `997 ms` | 탈락 |
| `retry_delay 6`, `retry_count 10` | RF 튜닝 실험 | 약 `16% loss`, avg 약 `748 ms` | 탈락 |
| TX recovery 에서 강제 `startListening()` 포함 | 회귀 실험 | 약 `22% loss`, avg 약 `474 ms` | recovery 방향이 오히려 성능 악화 |
| TX recovery 를 `flush_tx + gap` 만 유지 | 현재 | `0% loss`, avg 약 `331 ms` | 현재 가장 무난한 상태 |
| fairness 1차 적용 | peer pending 시 coord local DATA burst 2개 후 `Grant` 우선 | 양방향 `256B @ 10k` 경쟁에서도 양측 `0% loss` | 역방향 starvation 완화 방향 확인 |

### 실험 명령 요약

아래 명령들은 실제로 Coordinator(`192.168.10.1`)와 Peer(`192.168.10.2`)에서 사용한 테스트 예시입니다.

| 목적 | Coordinator 에서 실행 | Peer 에서 실행 |
| --- | --- | --- |
| 기본 ping | `ping 192.168.10.2` | - |
| payload ping | `ping 192.168.10.2 -s 32`
`ping 192.168.10.2 -s 128`
`ping 192.168.10.2 -s 256` | - |
| TCP echo 확인 | `echo "hello" | nc 192.168.10.2 12345`
`yes test | head -c 4096 | nc 192.168.10.2 12345` | `rm -f /tmp/nc_fifo && mkfifo /tmp/nc_fifo`
`while true; do cat /tmp/nc_fifo | nc -l 12345 | tee /tmp/nc_fifo; done` |
| UDP 단방향 (`COORD -> PEER`) | `iperf3 -c 192.168.10.2 -u -l 64 -b 10k -t 20`
`iperf3 -c 192.168.10.2 -u -l 128 -b 10k -t 20`
`iperf3 -c 192.168.10.2 -u -l 256 -b 10k -t 20`
`iperf3 -c 192.168.10.2 -u -l 256 -b 15k -t 20`
`iperf3 -c 192.168.10.2 -u -l 256 -b 20k -t 20` | `iperf3 -s` |
| UDP reverse (`PEER -> COORD`) | `iperf3 -s` | `iperf3 -c 192.168.10.1 -u -l 256 -b 10k -t 20` |
| UDP 양방향 경쟁 | `iperf3 -c 192.168.10.2 -u -l 256 -b 10k -t 20` | `iperf3 -c 192.168.10.1 -u -l 256 -b 10k -t 20` |
| 테스트 영상 송신 | `ffmpeg -i udp://0.0.0.0:5000 -t 60 -c copy out.ts` | `ffmpeg -re -f lavfi -i testsrc=size=128x96:rate=1 -vf format=gray -c:v libx264 -preset ultrafast -tune zerolatency -g 1 -bf 0 -b:v 8k -maxrate 8k -bufsize 4k -f mpegts udp://192.168.10.1:5000` |

### Ping 세부 결과

현재 기준선 조합에서 측정한 대표값: 

| 테스트 | 결과 |
| --- | --- |
| 기본 ping | `0% loss`, 평균 RTT 약 `198 ms` |
| `ping -s 32` | `0% loss`, 평균 RTT 약 `146 ms` |
| `ping -s 128` | `0% loss`, 평균 RTT 약 `262 ms` |
| `ping -s 256` | 최근 안정 구간에서 `0% loss`, 평균 RTT 약 `331 ms` |

해석:
- 작은 payload는 안정적입니다.
- `128B`도 현재는 안정적으로 수신됩니다.
- `256B`도 현재 조합에서는 `0% loss`로 안정화되는 구간을 확인했습니다. 다만 `send_fail` 자체는 아직 남아 있으므로 장시간/고부하 상황에서는 지연 증가 가능성이 있습니다.

### TCP 간단 검증

| 테스트 | 결과 |
| --- | --- |
| `nc` echo | `4 KiB` 수준 데이터 왕복 확인 |
| 작은 TCP interactive / 지속 전송 | 가능 |
| 더 큰 지속 전송 | 지연 누적 가능성 있음 |

### UDP 실측

`iperf3 -u` 기준 측정:

| 테스트 | 결과 | 비고 |
| --- | --- | --- |
| `64B @ 10k` | sender `0% loss` | 안정적 |
| `128B @ 10k` | sender `0% loss` | 안정적 |
| `256B @ 10k` | receiver 약 `1% loss`, jitter 약 `55 ms` | 저속 운용 가능 |
| `256B @ 10k` reverse (`PEER -> COORD`) | receiver `0% loss`, jitter 약 `32.4 ms`, completion time 약 `20.1 s` | 단방향 reverse 경로는 더 양호 |
| `256B @ 10k` bidirectional 경쟁 | `COORD -> PEER`: receiver `0% loss`, jitter 약 `73.7 ms`, completion time 약 `31.7 s`
`PEER -> COORD`: receiver `0% loss`, jitter 약 `11.8 ms`, completion time 약 `21.8 s` | fairness 1차 적용 후 양방향 경쟁에서도 유지 |
| `256B @ 15k` | receiver `0.68% loss`, jitter 약 `134 ms`, completion time 약 `40.5 s` | 가능하지만 지연 증가 |
| `256B @ 20k` | receiver 약 `1% loss`, jitter 약 `179 ms`, completion time 약 `54.7 s` | 실시간성 크게 악화 |

해석:
- 현재 링크는 `256B`에서도 낮은 loss로 버틸 수 있습니다.
- reverse 단독(`PEER -> COORD`)과 fairness 적용 후 양방향 경쟁 모두에서 `10k` 수준은 유지되었습니다.
- 하지만 bitrate를 올리면 손실보다 먼저 queue buildup 과 latency inflation 이 커집니다.
- 현재 기준으로 `256B payload`의 안정 운용점은 대략 `10k` 부근, `15k`는 가능한 대신 지연 증가를 감수하는 구간으로 보는 것이 합리적입니다.

### 현재 단계 평가

- 링크 안정화와 MAC 신뢰성 확보는 상당 부분 달성됨
- 작은 UDP/TCP는 안정화 단계에 진입
- `libgpiod` 기반 IRQ와 `250us` pacing은 현재 기준 채택 조합
- fairness 1차 적용은 양방향 경쟁에서 의미 있는 개선 후보로 보임
- 초저비트레이트 테스트 영상(`128x96`, `1fps`, 대략 `8~11 kbit/s`)은 수신 저장 후 재생 가능
- 현재 남은 핵심 과제는 대역폭 확대보다는 지연 감소와 `send_fail` 완화

### 현재 한계

- `send_fail`과 `rx_timeout`은 여전히 누적되며, 링크는 이를 재전송으로 간신히 흡수하는 상태입니다.
- frame type별 계측상 coordinator 는 `Grant/Data`, peer 는 `Ack` 쪽 failure 비중이 큽니다.
- 긴 연속 스트림은 `27B` 조각으로 많이 쪼개져 MAC 왕복 오버헤드가 큽니다.
- `256B payload`는 `10k` 부근이 안정 운용점이고, `15k~20k`로 올리면 손실보다 먼저 지연과 completion time 이 악화됩니다.
- reverse(`PEER -> COORD`)가 `COORD -> PEER`보다 여전히 더 좋은 경향이 있습니다.
- 영상은 가능하더라도 현재는 일반적인 스트리밍이 아니라 초저해상도/저fps feasibility 수준입니다.
- 따라서 다음 튜닝 방향은 RF/IRQ 미세조정보다 control-frame 오버헤드와 `send_fail` 완화에 맞추는 것이 적절합니다.
