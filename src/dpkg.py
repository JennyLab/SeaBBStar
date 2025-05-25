import asyncio
from dkg.providers import AsyncBlockchainProvider, AsyncNodeHTTPProvider
from dkg import AsyncDKG

async def main():
    node_provider = AsyncNodeHTTPProvider(
        endpoint_uri="http://localhost:8900",
        api_version="v1",
    )

    # make sure that you have PRIVATE_KEY in .env so the blockchain provider can load it
    blockchain_provider = AsyncBlockchainProvider(
        Environments.DEVELOPMENT.value,
        BlockchainIds.HARDHAT_1.value,
    )

    dkg = AsyncDKG(
        node_provider,
        blockchain_provider,
        config={"max_number_of_retries": 300, "frequency": 2},
    )
    
if __name__ == "__main__":
    asyncio.run(main())
