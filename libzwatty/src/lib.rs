use std::time::Duration;

use crate::ftms::{BleWrapper, FtmsError};

pub mod ftms;
mod recording;

pub async fn hello_zwatty() -> Result<(), FtmsError> {
    println!("Hello from libzwatty!");
    let ble = BleWrapper::try_new().await?;
    ble.start_scan().await;
    tokio::time::sleep(Duration::from_secs(1)).await;
    ble.get_devices().await?;
    ble.stop_scan().await;
    Ok(())
}

#[cfg(test)]
mod test {
    use super::*;

    #[tokio::test]
    async fn test_anything() {
        let res = hello_zwatty().await;
        println!("{:?}", res)
    }
}
