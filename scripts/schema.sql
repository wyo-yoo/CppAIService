CREATE TABLE IF NOT EXISTS users (
    id INT NOT NULL AUTO_INCREMENT PRIMARY KEY,
    username VARCHAR(255) NOT NULL UNIQUE,
    password VARCHAR(255) NOT NULL
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

CREATE TABLE IF NOT EXISTS chat_message (
    id INT NOT NULL COMMENT 'User ID, references users.id',
    username VARCHAR(255) NOT NULL,
    session_id VARCHAR(64) NOT NULL,
    is_user TINYINT NOT NULL,
    content MEDIUMTEXT NOT NULL,
    ts BIGINT NOT NULL,
    KEY idx_chat_history (id, session_id, ts)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;

-- ChatServer creates chat_sessions during startup.
