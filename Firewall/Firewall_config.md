# pfSense Firewall Configuration Instructions

## 1. Aliases Setup
Navigate to **Firewall > Aliases > IP**. These aliases map names to IP addresses for centralized management.

| Alias Name | IP Address | Description |
| :--- | :--- | :--- |
| `ADMIN_MGMT` | `<IP>` | Authorized IP for pfSense Web GUI access |
| `Central_EMQX` | `<IP>` | Central EMQX server IP |
| `Firewall_LAN` | `<IP>` | Firewall LAN interface (Gateway IP) |
| `Mosquitto_Host` | `<IP>` | Mosquitto service IP (on PI) |
| `VEGA_IoT_Device` | `<IP>` | All wireless AP IoT devices |
| `Wazuh_manager` | `<IP>` | Wazuh manager IP for raw telemetry |

---

## 2. Firewall Rules

### LAN Interface
*Rules should be ordered by priority.*

*   **[+] Pass:** IPv4 TCP `Mosquitto_Host` → `Central_EMQX`; *Allow Mosquitto PI to external EMQX (WAN)*
*   **[+] Pass:** IPv4 TCP `ADMIN_MGMT` → `Firewall_LAN`; *Allow System Admin access to pfSense Web GUI*
*   **[+] Pass:** IPv4 ANY `Mosquitto_Host` → `Firewall_LAN`; *Allow Mosquitto to access Firewall for WAN/DNS*
*   **[+] Pass:** IPv4 ANY `Mosquitto_Host` → `Wazuh_manager`; *Allow Mosquitto to send raw IoT telemetry to Wazuh*
*   **[+] Pass:** IPv4 TCP `VEGA_IoT_Device` → `Mosquitto_Host`; *Allow IoT devices to communicate with PI Mosquitto*
*   **[+] Block:** IPv4 ANY `LAN subnets` → `Firewall_LAN`; *Block Firewall access from overall LAN*
*   **[+] Block:** IPv4 ICMP `LAN subnets` → `ANY`; *Prevent LAN ping to WAN*
*   **[+] Block:** IPv4 ANY `LAN subnets` → `LAN subnets`; *Prevent inter-LAN communication*

### WAN Interface
*Rules should be ordered by priority.*

*   **[+] Block:** IPv4 ICMP `ANY` → `LAN subnets`; *Prevent WAN to LAN pinging*
