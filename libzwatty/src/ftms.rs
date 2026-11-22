use btleplug::{
    api::{Central, Manager as _, ScanFilter},
    platform::{Adapter, Manager},
};

use thiserror::Error;
// use uuid::Uuid;

// const FTMS_UUID: Uuid = Uuid::from_u128(0x00001826_0000_1000_8000_00805f9b34fb);
// const INDOOR_BIKE_DATA_UUID: Uuid = Uuid::from_u128(0x00002ad2_0000_1000_8000_00805f9b34fb);

#[derive(Debug, Error)]
pub enum FtmsError {
    #[error("Btle error: {0}")]
    Btle(#[from] btleplug::Error),

    #[error("No ble adapter found")]
    NoAdapter,
}

pub struct BleWrapper {
    adapter: Adapter,
}

impl BleWrapper {
    pub async fn try_new() -> Result<Self, FtmsError> {
        let manager = Manager::new().await?;

        // Get the first Bluetooth adapter
        let adapters = manager.adapters().await?;
        let central = adapters.into_iter().next().ok_or(FtmsError::NoAdapter)?;

        Ok(Self { adapter: central })
    }

    pub async fn start_scan(&self) -> Result<(), FtmsError> {
        self.adapter.start_scan(ScanFilter::default()).await?;
        Ok(())
    }

    pub async fn stop_scan(&self) -> Result<(), FtmsError> {
        self.adapter.stop_scan().await?;
        Ok(())
    }

    pub async fn get_devices(&self) -> Result<(), FtmsError> {
        // Start scanning for devices

        let peripherals = self
            .adapter
            .peripherals()
            .await
            .expect("no peripherals found");

        println!("{:?}", peripherals);

        Ok(())
    }
}
