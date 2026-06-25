// X1000Viewer.swift — Full-screen G1000 display receiver for iPad
//
// Receives UDP JPEG frames from X1000_display plugin on PC.
// Reassembles multi-chunk frames, displays at ~30fps.
//
// Setup: change LISTEN_PORT to match Plugin.cpp push_port (default 9000).
//
// To install without App Store (free Apple ID):
//   1. Open Xcode → File → New → Project → iOS App
//   2. Replace ContentView.swift content with this file
//   3. Connect iPad, select it as target, press Run
//   Xcode will ask to trust the developer on the iPad (Settings → General →
//   VPN & Device Management → trust your Apple ID)

import SwiftUI
import Network

// ---------------------------------------------------------------------------
// UDP frame protocol constants (must match DisplayStreamer.cpp)
// ---------------------------------------------------------------------------

let LISTEN_PORT: UInt16 = 9000
let MAGIC_FRAME: UInt32 = 0x464B3158  // "X1KF"
let MAGIC_CHUNK: UInt32 = 0x434B3158  // "X1KC"

// ---------------------------------------------------------------------------
// Frame reassembler — collects chunks, fires callback when complete
// ---------------------------------------------------------------------------

class FrameReassembler {
    struct PendingFrame {
        var chunks:    [UInt32: Data] = [:]
        var totalChunks: UInt32 = 0
    }

    private var pending: [UInt32: PendingFrame] = [:]
    var onFrame: ((Data) -> Void)?

    func receive(packet: Data) {
        guard packet.count >= 12 else { return }

        let magic = packet.uint32(at: 0)

        if magic == MAGIC_FRAME {
            // Single-packet frame
            guard packet.count >= 12 else { return }
            let dataLen = Int(packet.uint32(at: 8))
            guard packet.count >= 12 + dataLen else { return }
            let jpeg = packet.subdata(in: 12..<(12 + dataLen))
            onFrame?(jpeg)

        } else if magic == MAGIC_CHUNK {
            guard packet.count >= 20 else { return }
            let seq       = packet.uint32(at: 4)
            let chunkIdx  = packet.uint32(at: 8)
            let chunkTot  = packet.uint32(at: 12)
            let dataLen   = Int(packet.uint32(at: 16))
            guard packet.count >= 20 + dataLen else { return }

            let chunkData = packet.subdata(in: 20..<(20 + dataLen))

            if pending[seq] == nil {
                pending[seq] = PendingFrame()
            }
            pending[seq]!.chunks[chunkIdx] = chunkData
            pending[seq]!.totalChunks = chunkTot

            // Check if complete
            if UInt32(pending[seq]!.chunks.count) == chunkTot {
                var jpeg = Data()
                for i in 0..<chunkTot {
                    if let chunk = pending[seq]!.chunks[i] {
                        jpeg.append(chunk)
                    }
                }
                pending.removeValue(forKey: seq)
                // Clean up old pending frames (stale)
                let staleThreshold = seq > 5 ? seq - 5 : 0
                pending = pending.filter { $0.key >= staleThreshold }
                onFrame?(jpeg)
            }
        }
    }
}

extension Data {
    func uint32(at offset: Int) -> UInt32 {
        var value: UInt32 = 0
        withUnsafeMutableBytes(of: &value) { ptr in
            copyBytes(to: ptr, from: offset..<(offset + 4))
        }
        return UInt32(littleEndian: value)
    }
}

// ---------------------------------------------------------------------------
// UDP listener
// ---------------------------------------------------------------------------

class UDPReceiver: ObservableObject {
    @Published var currentImage: UIImage? = nil

    private var connection: NWConnection?
    private var listener:   NWListener?
    private let reassembler = FrameReassembler()
    private let queue = DispatchQueue(label: "udp.recv")

    init() {
        reassembler.onFrame = { [weak self] jpeg in
            if let img = UIImage(data: jpeg) {
                DispatchQueue.main.async {
                    self?.currentImage = img
                }
            }
        }
        startListening()
    }

    func startListening() {
        let params = NWParameters.udp
        params.allowLocalEndpointReuse = true

        do {
            listener = try NWListener(using: params,
                                      on: NWEndpoint.Port(rawValue: LISTEN_PORT)!)
        } catch {
            print("Listener create failed: \(error)")
            return
        }

        listener?.newConnectionHandler = { [weak self] conn in
            guard let self = self else { return }
            self.connection = conn
            conn.start(queue: self.queue)
            self.receiveLoop(conn)
        }

        listener?.start(queue: queue)
        print("X1000Viewer: listening on UDP :\(LISTEN_PORT)")
    }

    private func receiveLoop(_ conn: NWConnection) {
        conn.receiveMessage { [weak self] data, _, _, error in
            if let data = data, !data.isEmpty {
                self?.reassembler.receive(packet: data)
            }
            if error == nil {
                self?.receiveLoop(conn)
            }
        }
    }
}

// ---------------------------------------------------------------------------
// SwiftUI view — full screen, no UI chrome
// ---------------------------------------------------------------------------

struct ContentView: View {
    @StateObject private var receiver = UDPReceiver()

    var body: some View {
        ZStack {
            Color.black.ignoresSafeArea()

            if let img = receiver.currentImage {
                Image(uiImage: img)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
                    .ignoresSafeArea()
            } else {
                VStack(spacing: 16) {
                    ProgressView()
                        .tint(.white)
                    Text("Waiting for X1000 stream…")
                        .foregroundColor(.white)
                        .font(.system(size: 16, weight: .medium))
                    Text("UDP :\(LISTEN_PORT)")
                        .foregroundColor(.gray)
                        .font(.system(size: 12))
                }
            }
        }
        .statusBarHidden(true)
        .persistentSystemOverlays(.hidden)
    }
}

@main
struct X1000ViewerApp: App {
    var body: some Scene {
        WindowGroup {
            ContentView()
        }
    }
}
