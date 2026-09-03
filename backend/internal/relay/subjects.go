package relay

import "strings"

// FrameSubject is the live-only NATS subject used for one camera's JPEGs.
func FrameSubject(mac string) string {
	return "cams." + mac + ".frames"
}

// FrameMAC extracts the camera identity from a frame subject. Keeping this
// parser next to the subject builder prevents viewers from routing by payload.
func FrameMAC(subject string) (string, bool) {
	parts := strings.Split(subject, ".")
	if len(parts) != 3 || parts[0] != "cams" || parts[2] != "frames" || !validMAC(parts[1]) {
		return "", false
	}
	return parts[1], true
}

func validMAC(mac string) bool {
	if len(mac) != 12 {
		return false
	}
	for _, c := range mac {
		if !(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f') {
			return false
		}
	}
	return true
}
