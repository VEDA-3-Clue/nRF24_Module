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