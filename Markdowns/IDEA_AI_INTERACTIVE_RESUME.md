# Project Idea: AI-Powered Interactive Resume

*Saved Nov 17, 2026. Not started — to be developed further soon. Pasted verbatim below.*

Build a personal portfolio website featuring an AI chatbot that recruiters can interact with to learn about your work experience, technical skills, education, and personal projects.

The chatbot would use Retrieval-Augmented Generation (RAG) with Amazon Bedrock. RAG is a technique that allows an LLM to access external knowledge without needing to retrain the model.

You would create a knowledge base containing detailed information about your professional background. When a recruiter asks a question, the system searches the knowledge base for relevant information and provides it to the LLM as context. The LLM then generates a response grounded in your actual experience.

For example, a recruiter could ask, "What experience do you have with distributed systems?" and receive a detailed response referencing your relevant projects, contributions, and technical decisions.

Suggested tech stack:

- AWS Amplify — Website hosting
- API Gateway and Lambda — Backend API
- Amazon Bedrock — LLM inference
- Bedrock Knowledge Bases — RAG implementation
- Amazon S3 — Resume and project documentation
- S3 Vectors — Vector search and storage

The project provides recruiters with an interactive way to explore your qualifications while demonstrating your hands-on knowledge of generative AI, RAG, cloud architecture, and software engineering.
