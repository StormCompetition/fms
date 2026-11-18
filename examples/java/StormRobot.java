// Example STORM FMS robot client, protocol version 1. Java 17 or later.
//
// This is a starting point, not a robot. It connects to the FMS, authenticates, sends
// heartbeats, and tells your code when the robot may and may not move.
//
// Usage: STORM_TOKEN=<64 hex chars> java StormRobot.java --host 10.0.0.2 --team 12

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.nio.BufferUnderflowException;
import java.nio.ByteBuffer;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.SocketChannel;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CharsetDecoder;
import java.nio.charset.StandardCharsets;
import java.util.Locale;
import java.util.Map;
import java.util.function.Consumer;

public class StormRobot {

    static final String CLIENT_NAME = "storm-example-java";
    static final String CLIENT_VERSION = "1.0";

    static final int MAGIC = 0x5354524D; // "STRM"
    static final byte VERSION = 1;
    static final int HEADER_SIZE = 12; // magic u32, version u8, type u8, flags u16, length u16, sequence u16
    static final int MAX_PAYLOAD = 1024;

    // Message types.
    static final int HELLO = 0x01, HEARTBEAT = 0x02, GOODBYE = 0x03;
    static final int WELCOME = 0x81, REJECT = 0x82, MATCH_INFO = 0x83, PHASE_CHANGE = 0x84;
    static final int MOVE_AUTH = 0x85, SCORE_UPDATE = 0x86, STATUS = 0x87, ERROR = 0x8F;

    static final Map<Integer, String> PHASES = Map.of(
            0, "IDLE",
            1, "LOADED",
            2, "PRE_MATCH",
            3, "AUTONOMOUS",
            4, "REF_PAUSE",
            5, "TELEOP",
            6, "ENDED",
            7, "FINALIZED",
            8, "ABORTED"
    );

    static final Map<Integer, String> AUTH_REASONS = Map.of(
            1, "phase",
            2, "estop",
            3, "admin_disable",
            4, "admin_enable",
            5, "loss_of_signal",
            6, "disqualified",
            7, "field_fault",
            8, "no_match"
    );

    static final Map<Integer, String> REJECT_REASONS = Map.of(
            1, "unknown_team",
            2, "inactive_team",
            3, "bad_token",
            4, "ip_not_allowed",
            5, "bad_version",
            6, "malformed"
    );
    static final Map<Integer, String> ERROR_CODES = Map.of(
            1, "unexpected_message",
            2, "unknown_type",
            3, "malformed",
            4, "replaced",
            5, "revoked"
    );
    static final Map<Integer, String> ALLIANCES = Map.of(
            1, "red",
            2, "blue"
    );

    static final byte GOODBYE_SHUTTING_DOWN = 1;

    static final long MS = 1_000_000;
    static final long LOS_CAP = 1000 * MS;
    static final long RECONNECT_AFTER = 3000 * MS;
    static final int CONNECT_TIMEOUT_MS = 2000;
    static final long SEND_TIMEOUT = 1000 * MS;

    static class Malformed extends IOException {
        private static final long serialVersionUID = 1;

        Malformed(String message) {
            super(message);
        }
    }

    static int u8(ByteBuffer b) {
        return Byte.toUnsignedInt(b.get());
    }

    static int u16(ByteBuffer b) {
        return Short.toUnsignedInt(b.getShort());
    }

    static long u32(ByteBuffer b) {
        return Integer.toUnsignedLong(b.getInt());
    }

    static boolean bool(ByteBuffer b) throws Malformed {
        int v = u8(b);
        if (v > 1) {
            throw new Malformed("boolean field is " + v);
        }

        return v == 1;
    }

    static final CharsetDecoder UTF8 = StandardCharsets.UTF_8.newDecoder();

    static String string(ByteBuffer b) throws Malformed {
        byte[] s = new byte[u8(b)];
        b.get(s);

        try {
            return UTF8.decode(ByteBuffer.wrap(s)).toString();
        } catch (CharacterCodingException e) {
            throw new Malformed("invalid UTF-8");
        }
    }

    static byte[] bytes(ByteBuffer b) {
        byte[] s = new byte[u16(b)];
        b.get(s);

        return s;
    }

    static void putString(ByteBuffer b, String s) {
        byte[] utf8 = s.getBytes(StandardCharsets.UTF_8);
        if (utf8.length > 255) {
            throw new IllegalArgumentException("string longer than 255 bytes");
        }
        b.put((byte) utf8.length).put(utf8);
    }

    static void print(String format, Object... args) {
        System.out.println(String.format(Locale.ROOT, format, args));
    }

    final String host;
    final int port;
    final int team;
    final String token;

    final Selector selector;
    final ByteBuffer in = ByteBuffer.allocate(HEADER_SIZE + MAX_PAYLOAD);
    final ByteBuffer out = ByteBuffer.allocate(HEADER_SIZE + MAX_PAYLOAD);
    SocketChannel channel;
    int seq;

    volatile boolean enabled = false;
    String reason = "starting";
    volatile boolean stopping = false;

    boolean welcomed;
    long heartbeatInterval;
    long losTimeout;
    long lastRx;

    StormRobot(String host, int port, int team, String token) throws IOException {
        this.host = host;
        this.port = port;
        this.team = team;
        this.token = token;
        this.selector = Selector.open();
    }

    public boolean isEnabled() {
        return enabled;
    }

    protected void onMotion(boolean enabled, String reason) {
        print("motion: %s (%s)", enabled ? "enabled" : "disabled", reason);
    }

    protected void onPhase(String phase, long durationMs, long remainingMs) {
        print("phase: %s %.1f s", phase, remainingMs / 1000.0);
    }

    protected void onScore(int red, int blue) {
        print("score: red %d blue %d", red, blue);
    }

    void setMotion(boolean enabled, String reason) {
        if (enabled != this.enabled || !reason.equals(this.reason)) {
            this.enabled = enabled;
            this.reason = reason;
            onMotion(enabled, reason);
        }
    }

    void authorize(boolean enabled, int code) {
        String name = AUTH_REASONS.get(code);
        if (name == null) {
            setMotion(false, "unknown(" + code + ")");
        } else {
            setMotion(enabled, name);
        }
    }

    void send(int type, Consumer<ByteBuffer> body) throws IOException {
        out.clear().position(HEADER_SIZE);
        body.accept(out);
        int length = out.position() - HEADER_SIZE;
        out.putInt(0, MAGIC)
                .put(4, VERSION)
                .put(5, (byte) type)
                .putShort(6, (short) 0)
                .putShort(8, (short) length)
                .putShort(10, (short) seq)
                .flip();
		
        long deadline = System.nanoTime() + SEND_TIMEOUT;
        while (out.hasRemaining()) {
            if (channel.write(out) == 0) {
                if (System.nanoTime() - deadline > 0) {
                    throw new IOException("send timed out");
                }

                try {
                    Thread.sleep(1);
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                    throw new IOException("interrupted");
                }
            }
        }
        seq = (seq + 1) & 0xFFFF;
    }

    void sendHeartbeat() throws IOException {
        send(HEARTBEAT, (byteBuffer) -> {});
    }

    void close(boolean goodbye) {
        if (channel == null) {
            return;
        }

        if (goodbye) {
            try {
                send(GOODBYE, b -> b.put(GOODBYE_SHUTTING_DOWN));
            } catch (IOException e) {

            }
        }

        try {
            channel.close();
        } catch (IOException e) {
            // Nothing more to do.
        }
        channel = null;
    }

    void run() throws IOException {
        long backoffMs = 500;
        while (!stopping) {
            boolean ok = false;
            try {
                ok = session();
            } catch (IOException e) {
                print("connection: %s", e.getMessage() != null ? e.getMessage() : e.toString());
            } finally {
                setMotion(false, "disconnected");
                close(stopping);
            }

            if (ok) {
                backoffMs = 500;
            }

            long wake = System.nanoTime() + backoffMs * MS;
            for (long left; !stopping && (left = wake - System.nanoTime()) > 0;) {
                selector.select(Math.max(1, left / MS));
            }
            backoffMs = Math.min(backoffMs * 2, 5000);
        }
    }

    void stop() {
        stopping = true;
        selector.wakeup();
    }

    boolean session() throws IOException {
        channel = SocketChannel.open();
        channel.socket().connect(new InetSocketAddress(host, port), CONNECT_TIMEOUT_MS);
        channel.setOption(StandardSocketOptions.TCP_NODELAY, true);
        channel.configureBlocking(false);
        channel.register(selector, SelectionKey.OP_READ);
        seq = 0;
        in.clear();
        send(HELLO, b -> {
            b.putShort((short) team);
            putString(b, token);
            putString(b, CLIENT_NAME);
            putString(b, CLIENT_VERSION);
        });

        welcomed = false;
        heartbeatInterval = 100 * MS;
        losTimeout = LOS_CAP;
        lastRx = System.nanoTime();
        boolean heartbeating = false;
        long nextHeartbeat = 0;
        while (!stopping) {
            long now = System.nanoTime();
            long silent = now - lastRx;
            if (silent >= losTimeout) {
                setMotion(false, "loss_of_signal");
                if (silent >= RECONNECT_AFTER) {
                    print("connection: no message for %.1f s, reconnecting", silent / 1e9);
                    return welcomed;
                }
            }

            if (!heartbeating && welcomed) {
                heartbeating = true;
                nextHeartbeat = now;
            }

            if (heartbeating && now - nextHeartbeat >= 0) {
                sendHeartbeat();
                nextHeartbeat = Math.max(nextHeartbeat + heartbeatInterval, now);
            }

            long deadline = lastRx + (silent < losTimeout ? losTimeout : RECONNECT_AFTER);
            if (heartbeating && nextHeartbeat - deadline < 0) {
                deadline = nextHeartbeat;
            }

            selector.select(Math.max(1, (deadline - now + MS - 1) / MS)); // 0 would mean forever
            selector.selectedKeys().clear();

            int n = channel.read(in);
            if (n < 0) {
                print("connection: closed by server");
                return welcomed;
            }
            if (n > 0 && !process()) {
                return welcomed;
            }
        }
        return welcomed;
    }

    boolean process() throws IOException {
        in.flip();
        try {
            while (in.remaining() >= HEADER_SIZE) {
                int at = in.position();
                int magic = in.getInt(at);
                int version = Byte.toUnsignedInt(in.get(at + 4));
                int type = Byte.toUnsignedInt(in.get(at + 5));
                int length = Short.toUnsignedInt(in.getShort(at + 8));
                if (magic != MAGIC || length > MAX_PAYLOAD) {
                    throw new Malformed("bad frame header");
                }
                if (in.remaining() < HEADER_SIZE + length) {
                    break;
                }

                ByteBuffer payload = in.slice(at + HEADER_SIZE, length);
                in.position(at + HEADER_SIZE + length);
                if (version != VERSION) {
                    throw new Malformed("protocol version " + version);
                }

                lastRx = System.nanoTime();
                try {
                    if (!handle(type, payload)) {
                        return false;
                    }
                } catch (BufferUnderflowException e) {
                    throw new Malformed("payload too short");
                }
            }
        } finally {
            in.compact();
        }
        return true;
    }

    boolean handle(int type, ByteBuffer p) throws Malformed {
        switch (type) {
            case WELCOME -> {
                int teamId = u16(p);
                String teamName = string(p),
                        robotName = string(p),
                        gameId = string(p);
                int interval = u16(p),
                        timeout = u16(p);
                u16(p);
                heartbeatInterval = Math.max(interval, 10) * MS;
                losTimeout = Math.min(Math.max(timeout, 1) * MS, LOS_CAP);
                welcomed = true;
                print("connected: team %d \"%s\" robot \"%s\" game %s", teamId, teamName, robotName, gameId);
            }
            case REJECT -> {
                int localReason = u8(p);
                print("rejected: %s: %s", REJECT_REASONS.getOrDefault(localReason, "" + localReason), string(p));
                return false;
            }
            case ERROR -> {
                int code = u8(p);
                print("error: %s: %s", ERROR_CODES.getOrDefault(code, "" + code), string(p));
                if (code == 3 || code == 4 || code == 5) {
                    return false;

                }
            }
            case MOVE_AUTH -> {
                boolean on = bool(p);
                authorize(on, u8(p));
            }
            case STATUS -> {
                u8(p);
                boolean on = bool(p);
                int localReason = u8(p);
                u32(p);
                u16(p);
                u16(p);
                authorize(on, localReason);
            }
            case PHASE_CHANGE -> {
                int phase = u8(p);
                long duration = u32(p),
                        remaining = u32(p);
                onPhase(PHASES.getOrDefault(phase, "unknown(" + phase + ")"), duration, remaining);
            }
            case MATCH_INFO -> {
                long matchId = u32(p);
                int number = u16(p),
                        alliance = u8(p),
                        opponent = u16(p),
                        count = u8(p);
                for (int i = 0; i < count; i++) {
                    u8(p);
                    u32(p);
                }

                if (matchId == 0) {
                    print("match: none");
                } else {
                    print("match: %d (number %d) %s vs %s, %d phases",
                            matchId,
                            number,
                            ALLIANCES.getOrDefault(alliance, "" + alliance),
                            opponent == 0xFFFF ? "none" : "" + opponent,
                            count
                    );
                }
            }
            case SCORE_UPDATE -> {
                int red = u16(p),
                        blue = u16(p);
                bytes(p);
                onScore(red, blue);
            }
            default -> {
                // Any other message type is ignored, as the protocol requires.
            }
        }
        return true;
    }

    static void usage(String problem) {
        System.err.println("usage: java StormRobot.java [--host 127.0.0.1] [--port 5800] --team N [--token T]");
        System.err.println("error: " + problem);
        System.exit(2);
    }

    public static void main(String[] args) throws Exception {
        String host = "127.0.0.1";
        int port = 5800;
        int team = -1;
        String token = System.getenv().getOrDefault("STORM_TOKEN", "");
        for (int i = 0; i < args.length; i++) {
            String flag = args[i];
            if (i + 1 >= args.length) {
                usage(flag + " needs a value");
            }
            String value = args[++i];
            try {
                switch (flag) {
                    case "--host" ->
                        host = value;
                    case "--port" ->
                        port = Integer.parseInt(value);
                    case "--team" ->
                        team = Integer.parseInt(value);
                    case "--token" ->
                        token = value; // prefer the STORM_TOKEN environment variable
                    default ->
                        usage("unknown flag " + flag);
                }
            } catch (NumberFormatException e) {
                usage(flag + " must be a number");
            }
        }

        if (team < 0 || team > 0xFFFE) {
            usage("--team must be 0 to 65534");
        }
        if (token.isEmpty()) {
            usage("set STORM_TOKEN or pass --token");
        }

        StormRobot robot = new StormRobot(host, port, team, token);
        robot.onMotion(robot.enabled, robot.reason);

        Thread main = Thread.currentThread();
        Runtime.getRuntime().addShutdownHook(new Thread(() -> {
            robot.stop();

            try {
                main.join(3000);
            } catch (InterruptedException e) {
                // Exit anyway.
            }

            System.out.flush();
            Runtime.getRuntime().halt(0);
        }
        ));
        robot.run();
    }

}
