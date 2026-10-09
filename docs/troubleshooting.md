# Troubleshooting

This document show most common issues and possible solutions while using the device features.

- [Troubleshooting](#troubleshooting)
    - [Cannot See the Device on the Network](#cannot-see-the-device-on-the-network)
    - [Connection Drops or Times Out](#connection-drops-or-times-out)
    - [Upload Fails](#upload-fails)
    - [Saved Password Not Working](#saved-password-not-working)

### Cannot See the Device on the Network

**Problem:** Browser shows "Cannot connect" or "Site can't be reached"

**Solutions:**

1. Verify both devices are on the **same WiFi network**
   - Check your computer/phone WiFi settings
   - Confirm the CrossPoint Reader shows "Connected" status
2. Double-check the IP address
   - Make sure you typed it correctly
   - Include `http://` at the beginning
3. Try disabling VPN if you're using one
4. Some networks have "client isolation" enabled - check with your network administrator

### Connection Drops or Times Out

**Problem:** WiFi connection is unstable

**Solutions:**

1. Move closer to the WiFi router
2. Check the signal strength on the device's File Transfer screen: it should show at least two of the four bars (**Weak**) or better
3. Avoid interference from other devices
4. Try a different WiFi network if available

### Upload Fails

**Problem:** File upload doesn't complete or shows an error

**Solutions:**

1. "Cannot write to a protected location" means the file name starts with `.`, or the target folder is a system folder or a hidden one (hidden folders are writable only while **Show Hidden Files** is on). Rename the file or pick another folder
2. Ensure the file is a valid `.epub` file
3. Check that the SD card has enough free space
4. Try uploading a smaller file first to test
5. Refresh the browser page and try again

### Saved Password Not Working

**Problem:** Device fails to connect with saved credentials

**Solutions:**

1. When a saved network fails to connect, dismiss the error (Back or Confirm) to open **Network Options** (Cancel, Reset info, Forget)
2. Select **Forget** to remove the saved password. **Reset info** only clears the cached IP and DNS details and keeps the password
3. Reconnect and enter the password again
4. Choose to save the new password

You can open the same prompt from the network list: highlight a network with a saved password and press Left.
